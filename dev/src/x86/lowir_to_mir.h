#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "lowir/lowir_program.h"
#include "lowir/lowir_validate.h"
#include "mir_model.h"

// LowIR -> machine IR lowering. This is the PA28 backend boundary: all
// register assignment, staging, frame layout, and call ABI decisions are
// made here, and both the MIR dump and native emission consume the result.
//
// The register discipline is contractual (pa28 strict fixtures compare the
// dump register-for-register): pool r8, r9, rbx, r12..r15 in that order;
// values that live across a call take the callee-saved subset; parameters
// are pre-assigned pool slots in declaration order and stay pinned for the
// whole function; rax/rcx/rdx/rsi/rdi/r10/r11 are fixed staging registers.

mir_model::MirProgram LowerLowIRProgramToMir(const LowIRProgram & program,
                                             const LowIRProgramInfo & info,
                                             const std::string & target);

namespace lowir_to_mir {

// Spelling of a LowIR type in MIR dumps ("i64", "u16", "obj<3x1>", ...).
std::string SpellType(const LowIRType & type);

long long ParseIntLiteral(const LowIROperand & operand);
long long FrameSizeOf(const LowIRType & type);

mir_model::Operand MakeReg(X64Register reg);
mir_model::Operand MakeXmm(XmmRegister xmm);
mir_model::Operand MakeImm(long long value);
mir_model::Operand MakeSymbol(const std::string & name, bool global);
mir_model::Operand MakeLabel(const std::string & label);
mir_model::Operand MakeDeref(X64Register reg, long long offset);
mir_model::Operand MakeFloatImm(const LowIROperand & literal);

struct ValueUse
{
	enum Kind
	{
		USE_STORE_VALUE,   // store <ty> value, ...
		USE_RETURN_VALUE,  // return <ty> value
		USE_COPY_SOURCE,   // %d = copy <ty> value
		USE_CALL_ARG,      // call argument
		USE_BINARY_LHS,    // first operand of binary/cmp/unary/index/convert
		USE_OTHER          // addresses, branch conditions, everything else
	};

	int position = 0;
	Kind kind = USE_OTHER;
	int arg_index = -1;   // for USE_CALL_ARG
};

struct ValueInfo
{
	LowIRType type;
	bool is_param = false;
	int param_index = -1;
	int raw_references = 0;   // textual operand occurrences before rewrites
	int def_position = -1;
	std::vector<ValueUse> uses;
	bool cross_block = false;
	bool crosses_call = false;   // a call sits strictly inside the live range
	bool needs_frame = false;    // the value's own address is required

	int last_use() const
	{
		int last = def_position;
		for(size_t i = 0; i < uses.size(); i++)
			if(uses[i].position > last)
				last = uses[i].position;
		return last;
	}
};

// Where a value currently lives.
struct ValueLocation
{
	enum Kind
	{
		VL_NONE,        // dead or not yet defined
		VL_GPR,
		VL_XMM,
		VL_FRAME,       // frame home at frame_offset
		VL_ARG_REG,     // forwarded parameter still in its incoming register
		VL_SLOT_ADDR    // addr-of-slot temp, rematerialized at each use
	};

	Kind kind = VL_NONE;
	X64Register reg = XR_RAX;
	XmmRegister xmm = XMM_0;
	long long frame_offset = 0;
	std::string slot_name;
	bool also_in_rax = false;   // call/setcc results linger in rax
	bool prefer_home = false;   // param copy whose home is read while valid
};

struct SlotInfo
{
	LowIRType type;
	bool promoted = false;       // rewritten to register copies
	std::string alias_param;     // object slot aliased to a param home
	long long frame_offset = 0;
};

// Everything program-wide the per-function lowering needs to see.
struct ProgramFacts
{
	const LowIRProgramInfo * info = 0;
	// thread-local global name -> declared wrapper symbol
	std::map<std::string, std::string> tls_wrapper_of_global;
};

class FunctionLowering
{
public:
	static const int kPoolSize = 7;

	FunctionLowering(const LowIRFunction & function,
	                 const ProgramFacts & facts,
	                 mir_model::MirFunction & out);

	void Lower();

private:
	// -- analysis (lowir_to_mir_analyze.cpp)
	void Linearize();
	void PromoteSlots();
	void AnalyzeValues();
	void RecordOperandUse(const LowIROperand & operand, int position,
	                      ValueUse::Kind kind, int arg_index);
	void RecordInstructionUses(const LowIRInstruction & ins, int position);
	void MarkCallCrossings();
	void AliasObjectParamSlots();
	bool ParamUseIsForwardable(const ValueInfo & info,
	                           X64Register home) const;
	bool CallArgTargetsHome(int position, int arg_index,
	                        X64Register home) const;
	void RetimeSinkingCompares();

	// -- parameter and program scaffolding (lowir_to_mir_program.cpp)
	void PlanParams();
	bool PlanWideParam(const LowIRParam & param,
	                   mir_model::ParamBinding & binding,
	                   int & xmm, int gpr, long long & stack_offset);
	void EmitGprParamCopies(
		const std::vector<std::pair<int, X64Register> > & copies);
	void FinishFrame();

	// -- value and register machinery (lowir_to_mir_value.cpp)
	ValueInfo & value(const std::string & name);
	bool value_dies_here(const std::string & name) const;
	bool value_pinned(const std::string & name) const;
	bool alloc_pool_gpr(const std::string & name, X64Register & out_reg);
	X64Register alloc_gpr(const std::string & name);
	XmmRegister alloc_xmm(const std::string & name);
	long long alloc_frame_home(const std::string & name,
	                           const LowIRType & type,
	                           mir_model::FrameBinding::Kind kind);
	long long alloc_anonymous_spill();
	void release_after_use(const LowIRInstruction & ins);
	void release_value(const std::string & name);
	void note_callee_saved(X64Register reg);
	bool pool_reg_free(int pool_index) const;
	int pool_scan(bool callee_saved_only, bool skip_scratch) const;
	void invalidate_rax();
	bool operand_in_rax(const LowIROperand & operand) const;
	mir_model::Operand gpr_read(const LowIROperand & operand);
	mir_model::Operand address_operand(const LowIROperand & operand,
	                                   X64Register staging);
	mir_model::Operand frame_operand(long long offset) const;
	mir_model::Instruction & emit(mir_model::Instruction::Opcode opcode);
	void emit_mov(const mir_model::Operand & dst,
	              const mir_model::Operand & src);
	void emit_narrow_normalize(const LowIRType & type, X64Register reg);
	X64Register reload_to_pool(const LowIROperand & operand,
	                           const LowIRType & type);
	void RemoveDeadResultCopies();
	void emit_dest_copy(const std::string & dest, const LowIROperand & lhs,
	                    const LowIRType & type, bool normalize,
	                    X64Register & out_reg);

	// pending single-use loads folded into their consumer
	bool try_defer_load(const LowIRInstruction & ins, int position);
	bool operand_is_pending(const LowIROperand & operand) const;
	const LowIRInstruction * take_pending(const LowIROperand & operand);

	// -- instruction lowering (lowir_to_mir_inst.cpp)
	void LowerBlock(size_t block_index);
	void LowerInstruction(const LowIRInstruction & ins, int position);
	void LowerConst(const LowIRInstruction & ins);
	void LowerCopy(const LowIRInstruction & ins);
	void LowerAddr(const LowIRInstruction & ins);
	void LowerLoad(const LowIRInstruction & ins);
	void LowerStore(const LowIRInstruction & ins);
	void LowerIndex(const LowIRInstruction & ins);
	void LowerUnary(const LowIRInstruction & ins);
	void LowerBinary(const LowIRInstruction & ins);
	void LowerDivision(const LowIRInstruction & ins, bool modulus);
	void LowerShift(const LowIRInstruction & ins);
	void LowerCmpValue(const LowIRInstruction & ins);
	void LowerConvert(const LowIRInstruction & ins);
	void LowerFloatBinary(const LowIRInstruction & ins);
	void LowerFloatCmpValue(const LowIRInstruction & ins);

	// -- calls, control flow, atomics (lowir_to_mir_flow.cpp)
	void LowerCall(const LowIRInstruction & ins);
	void LowerCopyObj(const LowIRInstruction & ins);
	void LowerZeroInit(const LowIRInstruction & ins);
	void LowerAtomicLoad(const LowIRInstruction & ins);
	void LowerAtomicStore(const LowIRInstruction & ins);
	void LowerAtomicExchange(const LowIRInstruction & ins);
	void LowerAtomicCompareExchange(const LowIRInstruction & ins);
	void LowerAtomicAddFetch(const LowIRInstruction & ins);
	void LowerJump(const LowIRInstruction & ins);
	void LowerBranch(const LowIRInstruction & ins);
	void LowerSwitch(const LowIRInstruction & ins);
	void LowerReturn(const LowIRInstruction & ins);

	// shared pieces
	void assign_result_from_rax(const std::string & dest,
	                            const LowIRType & type, bool normalize);
	mir_model::Operand stage_store_value(const LowIROperand & operand,
	                                     const LowIRType & type);
	mir_model::Operand float_read(const LowIROperand & operand,
	                              const LowIRType & type);
	long long f80_result_home(const std::string & dest);
	bool storage_is_tls(const LowIROperand & operand) const;
	void emit_tls_addr(const std::string & global_name);
	const LowIRInstruction * fused_compare_for_branch(
		const LowIRInstruction & branch, int position, bool & invert) const;
	void emit_fused_compare(const LowIRInstruction & cmp,
	                        const std::string & true_label,
	                        const std::string & false_label);
	X86Condition integer_condition(const std::string & pred, bool sign) const;
	bool type_is_signed(const LowIRType & type) const;

	const LowIRFunction & function_;
	const ProgramFacts & facts_;
	mir_model::MirFunction & out_;

	// analysis results
	std::vector<const LowIRInstruction *> linear_;
	std::vector<int> block_first_position_;
	std::map<std::string, ValueInfo> values_;
	std::map<std::string, SlotInfo> slots_;
	std::map<std::string, LowIROperand> promoted_slot_value_;
	std::set<int> skip_positions_;        // promoted slot stores
	std::vector<int> call_positions_;
	bool touches_float_ = false;

	// live state
	std::map<std::string, ValueLocation> locations_;
	std::string pool_holder_[kPoolSize];  // value name; "" free; "*" hole
	std::string xmm_holder_[8];
	std::string rax_alias_;               // value also present in rax
	std::vector<X64Register> preserve_;
	long long frame_cursor_ = 0;
	long long residual_bytes_ = 0;
	int current_position_ = 0;
	std::string pending_load_name_;       // single-use load deferred to user
	const LowIRInstruction * pending_load_ = 0;
	bool arg_homes_clobbered_ = false;
	bool has_dead_source_spill_ = false;
	size_t current_block_ = 0;
	mir_model::MirBlock * mir_block_ = 0;
};

}  // namespace lowir_to_mir
