#include "stdafx.h"
#include "Utilities/Log.h"
#include "Emu/Cell/PPULLVMRecompiler.h"
#include "llvm/Support/Host.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/MC/MCDisassembler.h"

#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Vectorize.h"

#include "Emu/System.h"
#include "Emu/IdManager.h"

#include <sstream>

//#define PPU_LLVM_RECOMPILER_UNIT_TESTS 1           // Uncomment to enable tests
//#define PPU_LLVM_RECOMPILER_UNIT_TESTS_VERBOSE 1   // Uncomment to print everything (even for passed tests)

using namespace llvm;
using namespace ppu_recompiler_llvm;

#define VERIFY_INSTRUCTION_AGAINST_INTERPRETER(fn, tc, input, ...) \
VerifyInstructionAgainstInterpreter(fmt::Format("%s.%d", #fn, tc).c_str(), &Compiler::fn, &PPUInterpreter::fn, input, ##__VA_ARGS__)

#define VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(fn, s, n, ...) {  \
    PPUState input;                                                                 \
    for (int i = s; i < (n + s); i++) {                                             \
        input.SetRandom(0x10000);                                                   \
        VERIFY_INSTRUCTION_AGAINST_INTERPRETER(fn, i, input, ##__VA_ARGS__);        \
    }                                                                               \
}

/// Register state of a PPU
struct ppu_recompiler_llvm::PPUState {
	/// Floating point registers
	PPCdouble FPR[32];

	///Floating point status and control register
	FPSCRhdr FPSCR;

	/// General purpose reggisters
	u64 GPR[32];

	/// Vector purpose registers
	u128 VPR[32];

	/// Condition register
	CRhdr CR;

	/// Fixed point exception register
	XERhdr XER;

	/// Vector status and control register
	VSCRhdr VSCR;

	/// Link register
	u64 LR;

	/// Count register
	u64 CTR;

	/// SPR general purpose registers
	u64 SPRG[8];

	/// Time base register
	u64 TB;

	/// Memory block
	u32 address;
	u64 mem_block[64];

	void Load(PPUThread & ppu, u32 addr) {
		for (int i = 0; i < 32; i++) {
			FPR[i] = ppu.FPR[i];
			GPR[i] = ppu.GPR[i];
			VPR[i] = ppu.VPR[i];

			if (i < 8) {
				SPRG[i] = ppu.SPRG[i];
			}
		}

		FPSCR = ppu.FPSCR;
		CR = ppu.CR;
		XER = ppu.XER;
		VSCR = ppu.VSCR;
		LR = ppu.LR;
		CTR = ppu.CTR;
		TB = ppu.TB;

		address = addr;
		for (int i = 0; i < (sizeof(mem_block) / 8); i++) {
			mem_block[i] = vm::read64(address + (i * 8));
		}
	}

	void Store(PPUThread & ppu) {
		for (int i = 0; i < 32; i++) {
			ppu.FPR[i] = FPR[i];
			ppu.GPR[i] = GPR[i];
			ppu.VPR[i] = VPR[i];

			if (i < 8) {
				ppu.SPRG[i] = SPRG[i];
			}
		}

		ppu.FPSCR = FPSCR;
		ppu.CR = CR;
		ppu.XER = XER;
		ppu.VSCR = VSCR;
		ppu.LR = LR;
		ppu.CTR = CTR;
		ppu.TB = TB;

		for (int i = 0; i < (sizeof(mem_block) / 8); i++) {
			vm::write64(address + (i * 8), mem_block[i]);
		}
	}

	void SetRandom(u32 addr) {
		std::mt19937_64 rng;

		rng.seed((u32)std::chrono::high_resolution_clock::now().time_since_epoch().count());
		for (int i = 0; i < 32; i++) {
			FPR[i] = (double)rng();
			GPR[i] = rng();
			VPR[i]._f[0] = (float)rng();
			VPR[i]._f[1] = (float)(rng() & 0x7FFFFFFF);
			VPR[i]._f[2] = -(float)(rng() & 0x7FFFFFFF);
			VPR[i]._f[3] = -(float)rng();

			if (i < 8) {
				SPRG[i] = rng();
			}
		}

		FPSCR.FPSCR = (u32)rng();
		CR.CR = (u32)rng();
		XER.XER = 0;
		XER.CA = (u32)rng();
		XER.SO = (u32)rng();
		XER.OV = (u32)rng();
		VSCR.VSCR = (u32)rng();
		VSCR.X = 0;
		VSCR.Y = 0;
		LR = rng();
		CTR = rng();
		TB = rng();

		address = addr;
		for (int i = 0; i < (sizeof(mem_block) / 8); i++) {
			mem_block[i] = rng();
		}
	}

	std::string ToString() const {
		std::string ret;

		for (int i = 0; i < 32; i++) {
			ret += fmt::Format("GPR[%02d] = 0x%016llx  FPR[%02d] = %16g (0x%016llx)  VPR[%02d] = 0x%s [%s]\n", i, GPR[i], i, FPR[i]._double, FPR[i]._u64, i, VPR[i].to_hex().c_str(), VPR[i].to_xyzw().c_str());
		}

		for (int i = 0; i < 8; i++) {
			ret += fmt::Format("SPRG[%d] = 0x%016llx\n", i, SPRG[i]);
		}

		ret += fmt::Format("CR      = 0x%08x LR = 0x%016llx CTR = 0x%016llx TB=0x%016llx\n", CR.CR, LR, CTR, TB);
		ret += fmt::Format("XER     = 0x%016llx [CA=%d | OV=%d | SO=%d]\n", XER.XER, fmt::by_value(XER.CA), fmt::by_value(XER.OV), fmt::by_value(XER.SO));
		//ret += fmt::Format("FPSCR   = 0x%08x " // TODO: Uncomment after implementing FPSCR
		//                   "[RN=%d | NI=%d | XE=%d | ZE=%d | UE=%d | OE=%d | VE=%d | "
		//                   "VXCVI=%d | VXSQRT=%d | VXSOFT=%d | FPRF=%d | "
		//                   "FI=%d | FR=%d | VXVC=%d | VXIMZ=%d | "
		//                   "VXZDZ=%d | VXIDI=%d | VXISI=%d | VXSNAN=%d | "
		//                   "XX=%d | ZX=%d | UX=%d | OX=%d | VX=%d | FEX=%d | FX=%d]\n",
		//                   FPSCR.FPSCR,
		//                   fmt::by_value(FPSCR.RN),
		//                   fmt::by_value(FPSCR.NI), fmt::by_value(FPSCR.XE), fmt::by_value(FPSCR.ZE), fmt::by_value(FPSCR.UE), fmt::by_value(FPSCR.OE), fmt::by_value(FPSCR.VE),
		//                   fmt::by_value(FPSCR.VXCVI), fmt::by_value(FPSCR.VXSQRT), fmt::by_value(FPSCR.VXSOFT), fmt::by_value(FPSCR.FPRF),
		//                   fmt::by_value(FPSCR.FI), fmt::by_value(FPSCR.FR), fmt::by_value(FPSCR.VXVC), fmt::by_value(FPSCR.VXIMZ),
		//                   fmt::by_value(FPSCR.VXZDZ), fmt::by_value(FPSCR.VXIDI), fmt::by_value(FPSCR.VXISI), fmt::by_value(FPSCR.VXSNAN),
		//                   fmt::by_value(FPSCR.XX), fmt::by_value(FPSCR.ZX), fmt::by_value(FPSCR.UX), fmt::by_value(FPSCR.OX), fmt::by_value(FPSCR.VX), fmt::by_value(FPSCR.FEX), fmt::by_value(FPSCR.FX));
		//ret += fmt::Format("VSCR    = 0x%08x [NJ=%d | SAT=%d]\n", VSCR.VSCR, fmt::by_value(VSCR.NJ), fmt::by_value(VSCR.SAT)); // TODO: Uncomment after implementing VSCR.SAT

		for (int i = 0; i < (sizeof(mem_block) / 8); i += 2) {
			ret += fmt::Format("mem_block[%d] = 0x%016llx mem_block[%d] = 0x%016llx\n", i, mem_block[i], i + 1, mem_block[i + 1]);
		}

		return ret;
	}
};

#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS
static std::string StateDiff(PPUState const & recomp, PPUState const & interp) {
	std::string ret;

	for (int i = 0; i < 32; i++) {
		if (recomp.GPR[i] != interp.GPR[i]) {
			ret += fmt::Format("recomp: GPR[%02d] = 0x%016llx interp: GPR[%02d] = 0x%016llx\n", i, recomp.GPR[i], i, interp.GPR[i]);
		}
		if (recomp.FPR[i]._u64 != interp.FPR[i]._u64) {
			ret += fmt::Format("recomp: FPR[%02d] = %16g (0x%016llx) interp: FPR[%02d] = %16g (0x%016llx)\n", i, recomp.FPR[i]._double, recomp.FPR[i]._u64, i, interp.FPR[i]._double, interp.FPR[i]._u64);
		}
		if (recomp.VPR[i] != interp.VPR[i]) {
			ret += fmt::Format("recomp: VPR[%02d] = 0x%s [%s]\n", i, recomp.VPR[i].to_hex().c_str(), recomp.VPR[i].to_xyzw().c_str());
			ret += fmt::Format("interp: VPR[%02d] = 0x%s [%s]\n", i, interp.VPR[i].to_hex().c_str(), interp.VPR[i].to_xyzw().c_str());
		}
	}

	for (int i = 0; i < 8; i++) {
		if (recomp.SPRG[i] != interp.SPRG[i])
			ret += fmt::Format("recomp: SPRG[%d] = 0x%016llx interp: SPRG[%d] = 0x%016llx\n", i, recomp.SPRG[i], i, interp.SPRG[i]);
	}

	if (recomp.CR.CR != interp.CR.CR) {
		ret += fmt::Format("recomp: CR      = 0x%08x\n", recomp.CR.CR);
		ret += fmt::Format("interp: CR      = 0x%08x\n", interp.CR.CR);
	}
	if (recomp.LR != interp.LR) {
		ret += fmt::Format("recomp: LR = 0x%016llx\n", recomp.LR);
		ret += fmt::Format("interp: LR = 0x%016llx\n", interp.LR);
	}
	if (recomp.CTR != interp.CTR) {
		ret += fmt::Format("recomp: CTR = 0x%016llx\n", recomp.CTR);
		ret += fmt::Format("interp: CTR = 0x%016llx\n", interp.CTR);
	}
	if (recomp.TB != interp.TB) {
		ret += fmt::Format("recomp: TB = 0x%016llx\n", recomp.TB);
		ret += fmt::Format("interp: TB = 0x%016llx\n", interp.TB);
	}

	if (recomp.XER.XER != interp.XER.XER) {
		ret += fmt::Format("recomp: XER     = 0x%016llx [CA=%d | OV=%d | SO=%d]\n", recomp.XER.XER, fmt::by_value(recomp.XER.CA), fmt::by_value(recomp.XER.OV), fmt::by_value(recomp.XER.SO));
		ret += fmt::Format("interp: XER     = 0x%016llx [CA=%d | OV=%d | SO=%d]\n", interp.XER.XER, fmt::by_value(interp.XER.CA), fmt::by_value(interp.XER.OV), fmt::by_value(interp.XER.SO));
	}

	for (int i = 0; i < (sizeof(recomp.mem_block) / 8); i++) {
		if (recomp.mem_block[i] != interp.mem_block[i]) {
			ret += fmt::Format("recomp: mem_block[%d] = 0x%016llx\n", i, recomp.mem_block[i]);
			ret += fmt::Format("interp: mem_block[%d] = 0x%016llx\n", i, interp.mem_block[i]);
		}
	}

	return ret;
}
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS

#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS
static PPUThread      * s_ppu_state = nullptr;
static PPUInterpreter * s_interpreter = nullptr;
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS

template <class... Args>
void Compiler::VerifyInstructionAgainstInterpreter(const char * name, void (Compiler::*recomp_fn)(Args...), void (PPUInterpreter::*interp_fn)(Args...), PPUState & input_state, Args... args) {
#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS
	auto test_case = [&]() {
		(this->*recomp_fn)(args...);
	};
	auto input = [&]() {
		input_state.Store(*s_ppu_state);
	};
	auto check_result = [&](std::string & msg) {
		PPUState recomp_output_state;
		PPUState interp_output_state;

		recomp_output_state.Load(*s_ppu_state, input_state.address);
		input_state.Store(*s_ppu_state);
		(s_interpreter->*interp_fn)(args...);
		interp_output_state.Load(*s_ppu_state, input_state.address);

		if (interp_output_state.ToString() != recomp_output_state.ToString()) {
			msg = std::string("Input state:\n") + input_state.ToString() +
#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS_VERBOSE
				std::string("\nOutput state:\n") + recomp_output_state.ToString() +
				std::string("\nInterpreter output state:\n") + interp_output_state.ToString() +
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS_VERBOSE
				std::string("\nState diff:\n") + StateDiff(recomp_output_state, interp_output_state);
			return false;
		}

		return true;
	};
	RunTest(name, test_case, input, check_result);
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS
}

void Compiler::RunTest(const char * name, std::function<void()> test_case, std::function<void()> input, std::function<bool(std::string & msg)> check_result) {
#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS
	m_recompilation_engine.Log() << "Running test " << name << '\n';

	m_module = new llvm::Module("Module", *m_llvm_context);
	m_execute_unknown_function = (Function *)m_module->getOrInsertFunction("execute_unknown_function", m_compiled_function_type);
	m_execute_unknown_function->setCallingConv(CallingConv::X86_64_Win64);

	m_execute_unknown_block = (Function *)m_module->getOrInsertFunction("execute_unknown_block", m_compiled_function_type);
	m_execute_unknown_block->setCallingConv(CallingConv::X86_64_Win64);

	std::string targetTriple = "x86_64-pc-windows-elf";
	m_module->setTargetTriple(targetTriple);

	llvm::ExecutionEngine *execution_engine =
		EngineBuilder(std::unique_ptr<llvm::Module>(m_module))
		.setEngineKind(EngineKind::JIT)
		.setMCJITMemoryManager(std::unique_ptr<llvm::SectionMemoryManager>(new CustomSectionMemoryManager(m_executableMap)))
		.setOptLevel(llvm::CodeGenOpt::Aggressive)
		.setMCPU("nehalem")
		.create();
	m_module->setDataLayout(execution_engine->getDataLayout());

	llvm::FunctionPassManager *fpm = new llvm::FunctionPassManager(m_module);
	fpm->add(createNoAAPass());
	fpm->add(createBasicAliasAnalysisPass());
	fpm->add(createNoTargetTransformInfoPass());
	fpm->add(createEarlyCSEPass());
	fpm->add(createTailCallEliminationPass());
	fpm->add(createReassociatePass());
	fpm->add(createInstructionCombiningPass());
	fpm->add(new DominatorTreeWrapperPass());
	fpm->add(new MemoryDependenceAnalysis());
	fpm->add(createGVNPass());
	fpm->add(createInstructionCombiningPass());
	fpm->add(new MemoryDependenceAnalysis());
	fpm->add(createDeadStoreEliminationPass());
	fpm->add(new LoopInfo());
	fpm->add(new ScalarEvolution());
	fpm->add(createSLPVectorizerPass());
	fpm->add(createInstructionCombiningPass());
	fpm->add(createCFGSimplificationPass());
	fpm->doInitialization();

	// Create the function
	m_state.function = (Function *)m_module->getOrInsertFunction(name, m_compiled_function_type);
	m_state.function->setCallingConv(CallingConv::X86_64_Win64);
	auto arg_i = m_state.function->arg_begin();
	arg_i->setName("ppu_state");
	m_state.args[CompileTaskState::Args::State] = arg_i;
	(++arg_i)->setName("context");
	m_state.args[CompileTaskState::Args::Context] = arg_i;
	m_state.current_instruction_address = s_ppu_state->PC;

	auto block = BasicBlock::Create(*m_llvm_context, "start", m_state.function);
	m_ir_builder->SetInsertPoint(block);

	test_case();

	m_ir_builder->CreateRet(m_ir_builder->getInt32(0));

	std::stringstream logmsg;

	// Print the IR
	std::string        ir;
	raw_string_ostream ir_ostream(ir);
	m_state.function->print(ir_ostream);
	//m_recompilation_engine.Log() << "LLVM IR:" << ir;
	logmsg << "LLVM IR:" << ir;

	std::string        verify_results;
	raw_string_ostream verify_results_ostream(verify_results);
	if (verifyFunction(*m_state.function, &verify_results_ostream)) {
		//	m_recompilation_engine.Log() << "Verification Failed:\n" << verify_results << '\n';
		logmsg << "Verification Failed:\n" << verify_results << '\n';
		return;
	}

	// Optimize
	fpm->run(*m_state.function);

	// Print the optimized IR
	ir = "";
	m_state.function->print(ir_ostream);
	//m_recompilation_engine.Log() << "Optimized LLVM IR:" << ir;
	logmsg << "Optimized LLVM IR:" << ir;

	// Generate the function
	//MachineCodeInfo mci;
	execution_engine->finalizeObject();

	// Run the test
	input();
	auto executable = (Executable)execution_engine->getPointerToFunction(m_state.function);
	executable(s_ppu_state, 0);

	// Verify results
	std::string msg;
	bool        pass = check_result(msg);
	if (pass) {
#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS_VERBOSE
		m_recompilation_engine.Log() << logmsg.str() << "Test " << name << " passed\n" << msg << "\n";
#else
		m_recompilation_engine.Log() << "Test " << name << " passed\n";
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS_VERBOSE
	}
	else {
		m_recompilation_engine.Log() << logmsg.str() << "Test " << name << " failed\n" << msg << "\n";
	}

	delete fpm;
	delete m_module;
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS
}

void Compiler::RunAllTests() {
#ifdef PPU_LLVM_RECOMPILER_UNIT_TESTS
	s_ppu_state = Emu.GetIdManager().make_ptr<PPUThread>("Test Thread").get();
	PPUInterpreter interpreter(*s_ppu_state);
	s_interpreter = &interpreter;

	m_recompilation_engine.Log() << "Starting Unit Tests\n";

	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFVSCR, 0, 5, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTVSCR, 0, 5, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDCUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSHS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDSWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUBM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUHM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUHS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUWM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VADDUWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAND, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VANDC, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGSW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VAVGUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFSX, 0, 5, 0u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFSX, 5, 5, 0u, 3u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFUX, 0, 5, 0u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCFUX, 5, 5, 0u, 2u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPBFP_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQFP_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUB_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUH_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPEQUW_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGEFP_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTFP_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSB_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSH_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTSW_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUB_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUH_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW_, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCMPGTUW_, 5, 5, 0u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCTSXS, 0, 5, 0u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCTSXS, 5, 5, 0u, 3u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCTUXS, 0, 5, 0u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VCTUXS, 5, 5, 0u, 3u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VEXPTEFP, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VLOGEFP, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMADDFP, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXSB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXSH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXSW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMAXUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMHADDSHS, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMHRADDSHS, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINSB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINSH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINSW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMINUW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMLADDUHM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGHB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGHH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGHW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGLB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGLH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMRGLW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMMBM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMSHM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMSHS, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMUBM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMUHM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMSUMUHS, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULESB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULESH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULEUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULEUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULOSB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULOSH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULOUB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VMULOUH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VNMSUBFP, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VNOR, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VOR, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPERM, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKPX, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKSHSS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKSHUS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKSWSS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKSWUS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKUHUM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKUHUS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKUWUM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VPKUWUS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VREFP, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRFIM, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRFIN, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRFIP, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRFIZ, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRLB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRLH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRLW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VRSQRTEFP, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSEL, 0, 5, 0u, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSL, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSLB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSLDOI, 0, 5, 0u, 1u, 2u, 6u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSLH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSLO, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSLW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTB, 0, 5, 0u, 3u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTH, 0, 5, 0u, 3u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTISB, 0, 5, 0u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTISH, 0, 5, 0u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTISW, 0, 5, 0u, -12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSPLTW, 0, 5, 0u, 3u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSR, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRAB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRAH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRAW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRB, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRH, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRO, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSRW, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBFP, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBSBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBSHS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBSWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUBM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUHM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUHS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUWM, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUBUWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUMSWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUM2SWS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUM4SBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUM4SHS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VSUM4UBS, 0, 5, 0u, 1u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKHPX, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKHSB, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKHSH, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKLPX, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKLSB, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VUPKLSH, 0, 5, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(VXOR, 0, 5, 0u, 1u, 2u);
	// TODO: Rest of the vector instructions

	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLI, 0, 5, 1u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFIC, 0, 5, 1u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPLI, 0, 5, 1u, 0u, 7u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPLI, 5, 5, 1u, 1u, 7u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPI, 0, 5, 5u, 0u, 7u, -12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPI, 5, 5, 5u, 1u, 7u, -12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIC, 0, 5, 1u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIC_, 0, 5, 1u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDI, 0, 5, 1u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDI, 5, 5, 0u, 2u, 12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIS, 0, 5, 1u, 2u, -12345);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDIS, 5, 5, 0u, 2u, -12345);
	// TODO: BC
	// TODO: SC
	// TODO: B
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRF, 0, 5, 0u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRF, 5, 5, 6u, 2u);
	// TODO: BCLR
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRNOR, 0, 5, 0u, 7u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRANDC, 0, 5, 5u, 6u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ISYNC, 0, 5);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRXOR, 0, 5, 7u, 7u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRNAND, 0, 5, 3u, 4u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRAND, 0, 5, 1u, 2u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CREQV, 0, 5, 2u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CRORC, 0, 5, 3u, 4u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CROR, 0, 5, 6u, 7u, 0u);
	// TODO: BCCTR
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWIMI, 0, 5, 7u, 8u, 9u, 12u, 25u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWIMI, 5, 5, 21u, 22u, 21u, 18u, 24u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWINM, 0, 5, 7u, 8u, 9u, 12u, 25u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWINM, 5, 5, 21u, 22u, 21u, 18u, 24u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWNM, 0, 5, 7u, 8u, 9u, 12u, 25u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLWNM, 5, 5, 21u, 22u, 21u, 18u, 24u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORI, 0, 5, 25u, 29u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORIS, 0, 5, 7u, 31u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XORI, 0, 5, 0u, 19u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XORIS, 0, 5, 3u, 14u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDI_, 0, 5, 16u, 7u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDIS_, 0, 5, 23u, 21u, 12345u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDICL, 0, 5, 7u, 8u, 9u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDICL, 5, 5, 21u, 22u, 43u, 43u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDICR, 0, 5, 7u, 8u, 0u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDICR, 5, 5, 21u, 22u, 63u, 43u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDIC, 0, 5, 7u, 8u, 9u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDIC, 5, 5, 21u, 22u, 23u, 43u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDIMI, 0, 5, 7u, 8u, 9u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDIMI, 5, 5, 21u, 22u, 23u, 43u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDC_LR, 0, 5, 7u, 8u, 9u, 12u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(RLDC_LR, 5, 5, 21u, 22u, 23u, 43u, 1u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMP, 0, 5, 3u, 0u, 9u, 31u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMP, 5, 5, 6u, 1u, 23u, 14u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFC, 0, 5, 0u, 1u, 2u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFC, 5, 5, 0u, 1u, 2u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDC, 0, 5, 0u, 1u, 2u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDC, 5, 5, 0u, 1u, 2u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHDU, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHDU, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHWU, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHWU, 5, 5, 21u, 22u, 23u, 1u);
	// TODO: MFOCRF
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SLW, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SLW, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CNTLZW, 0, 5, 5u, 6u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CNTLZW, 5, 5, 5u, 6u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SLD, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SLD, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(AND, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(AND, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPL, 0, 5, 3u, 0u, 9u, 31u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CMPL, 5, 5, 6u, 1u, 23u, 14u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBF, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBF, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CNTLZD, 0, 5, 5u, 6u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(CNTLZD, 5, 5, 5u, 6u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDC, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ANDC, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHD, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHD, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHW, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULHW, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NEG, 0, 5, 7u, 8u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NEG, 5, 5, 21u, 22u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NOR, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NOR, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFE, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFE, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDE, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDE, 5, 5, 21u, 22u, 23u, 0u, 1u);
	// TODO: MTOCRF
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDZE, 0, 5, 7u, 8u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDZE, 5, 5, 21u, 22u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFZE, 0, 5, 7u, 8u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFZE, 5, 5, 21u, 22u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFME, 0, 5, 7u, 8u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SUBFME, 5, 5, 21u, 22u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLD, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLD, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDME, 0, 5, 7u, 8u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADDME, 5, 5, 21u, 22u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLW, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MULLW, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADD, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ADD, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EQV, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EQV, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XOR, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(XOR, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFSPR, 0, 5, 5u, 0x20u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFSPR, 5, 5, 5u, 0x100u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFSPR, 10, 5, 5u, 0x120u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFSPR, 15, 5, 5u, 0x8u);
	// TODO: MFTB
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORC, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(ORC, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(OR, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(OR, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVDU, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVDU, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVWU, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVWU, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTSPR, 0, 5, 0x20u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTSPR, 5, 5, 0x100u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTSPR, 10, 5, 0x120u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTSPR, 15, 5, 0x8u, 5u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NAND, 0, 5, 7u, 8u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(NAND, 5, 5, 21u, 22u, 23u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVD, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVD, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVW, 0, 5, 7u, 8u, 9u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(DIVW, 5, 5, 21u, 22u, 23u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRW, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRW, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRD, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRD, 5, 5, 5u, 6u, 7u, 1u);
	// TODO: SYNC
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAW, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAW, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAD, 0, 5, 5u, 6u, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAD, 5, 5, 5u, 6u, 7u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAWI, 0, 5, 5u, 6u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAWI, 5, 5, 5u, 6u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAWI, 10, 5, 5u, 6u, 22u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRAWI, 15, 5, 5u, 6u, 31u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRADI1, 0, 5, 5u, 6u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRADI1, 5, 5, 5u, 6u, 12u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRADI1, 10, 5, 5u, 6u, 48u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(SRADI1, 15, 5, 5u, 6u, 63u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EIEIO, 0, 5);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSH, 0, 5, 6u, 9u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSH, 5, 5, 6u, 9u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSB, 0, 5, 3u, 5u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSB, 5, 5, 3u, 5u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSW, 0, 5, 25u, 29u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(EXTSW, 5, 5, 25u, 29u, 1u);

	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FDIVS, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FSUBS, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FADDS, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FSQRTS, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FRES, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMULS, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMADDS, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMSUBS, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNMSUBS, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNMADDS, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB1, 0, 5, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB1, 5, 5, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB1, 10, 5, 25u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB1, 15, 5, 31u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRFS, 0, 5, 0u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRFS, 5, 5, 7u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRFS, 10, 5, 5u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MCRFS, 15, 5, 5u, 3u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB0, 0, 5, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB0, 5, 5, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB0, 10, 5, 25u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSB0, 15, 5, 31u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSFI, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSFI, 5, 5, 2u, 6u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSFI, 10, 5, 5u, 11u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSFI, 15, 5, 7u, 14u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MFFS, 0, 5, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSF, 0, 5, 0u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSF, 5, 5, 2u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSF, 10, 5, 5u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(MTFSF, 15, 5, 7u, 0u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCMPU, 0, 5, 5u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FRSP, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCTIW, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCTIWZ, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FDIV, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FSUB, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FADD, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FSQRT, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FSEL, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMUL, 0, 5, 0u, 1u, 2u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FRSQRTE, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMSUB, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMADD, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNMSUB, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNMADD, 0, 5, 0u, 1u, 2u, 3u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCMPO, 0, 5, 3u, 0u, 1u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNEG, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FMR, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FNABS, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FABS, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCTID, 0, 5, 0u, 1u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER_USING_RANDOM_INPUT(FCFID, 0, 5, 0u, 1u, 0u);

	PPUState input;
	input.SetRandom(0x10000);
	input.GPR[14] = 10;
	input.GPR[21] = 15;
	input.GPR[23] = 0x10000;
	input.mem_block[0] = 0x8877665544332211;

	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZ, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZ, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LBZUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZ, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZ, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZX, 1, input, 5u, 14u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(ECIWX, 0, input, 5u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(ECIWX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHZUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHA, 0, input, 5u, 0u, 0x100F0);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHA, 1, input, 5u, 14u, 0x100F0);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAU, 0, input, 5u, 14u, 0x100F0);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHAUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LHBRX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZ, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZ, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWZUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWA, 0, input, 5u, 0u, 0x100F0);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWA, 1, input, 5u, 14u, 0x100F0);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWAUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWBRX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LD, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LD, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDBRX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFS, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFS, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFSU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFSX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFSX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFSUX, 0, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFD, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFD, 1, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFDU, 0, input, 5u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFDX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFDX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LFDUX, 0, input, 5u, 14u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWARX, 0, input, 5u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LWARX, 1, input, 5u, 14u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDARX, 0, input, 5u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LDARX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LSWI, 0, input, 5u, 23u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LSWI, 1, input, 5u, 23u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LSWI, 2, input, 5u, 23u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LSWI, 3, input, 5u, 23u, 25u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LMW, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LMW, 1, input, 16u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVXL, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVXL, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSL, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSL, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSL, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSR, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSR, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVSR, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEBX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEBX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEBX, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEHX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEHX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEHX, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEWX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEWX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVEWX, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVLX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVLX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVLX, 2, input, 5u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVRX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVRX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(LVRX, 2, input, 5u, 21u, 23u);

	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STB, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STB, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDCX_, 0, input, 3u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDCX_, 1, input, 3u, 14u, 23u); unhandled unknown exception, new
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STBUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STH, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STH, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHX, 1, input, 3u, 14u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(ECOWX, 0, input, 3u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(ECOWX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STHBRX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STW, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STW, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVLX, 0, input, 0u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVLX, 1, input, 0u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVLX, 2, input, 0u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWBRX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STD, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STD, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWCX_, 0, input, 3u, 0u, 23u);
	//VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STWCX_, 1, input, 3u, 14u, 23u); unhandled unknown exception, new
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STDUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFS, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFS, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFSU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFSX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFSX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVRX, 0, input, 0u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVRX, 1, input, 0u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVRX, 2, input, 0u, 21u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFSUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFD, 0, input, 3u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFD, 1, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFDU, 0, input, 3u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFDX, 0, input, 3u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFDX, 1, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFDUX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STFIWX, 0, input, 3u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVXL, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVXL, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEBX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEBX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEHX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEHX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEWX, 0, input, 5u, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STVEWX, 1, input, 5u, 14u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STMW, 0, input, 5u, 0u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STMW, 1, input, 16u, 14u, 0x10000);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STSWI, 0, input, 5u, 23u, 0u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STSWI, 1, input, 5u, 23u, 2u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STSWI, 2, input, 5u, 23u, 7u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(STSWI, 3, input, 5u, 23u, 25u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(DCBZ, 0, input, 0u, 23u);
	VERIFY_INSTRUCTION_AGAINST_INTERPRETER(DCBZ, 1, input, 14u, 23u);

	m_recompilation_engine.Log() << "Finished Unit Tests\n";
	Emu.GetIdManager().remove<PPUThread>(s_ppu_state->get_id());
#endif // PPU_LLVM_RECOMPILER_UNIT_TESTS
}