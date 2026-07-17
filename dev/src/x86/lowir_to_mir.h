#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "x86/lowir_to_mir_model.h"

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

// One armed host-EH region discovered by the PA31 region dataflow
// (lowir_to_mir_eh.cpp): the landing-pad label (a dispatch/cleanup
// block, or a synthesized throw-payload abandon pad), the arming kind,
// the region armed beneath it, and the landing pad's catch clauses.
struct EhRegionPlan
{
	std::string landing_label;
	bool cleanup = false;
	bool synthetic = false;
	int parent = -1;
	std::vector<mir_model::HostEhClause> clauses;
};

class FunctionLowering
{
public:
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
	void MarkByAddressArgs();
	void AliasObjectParamSlots();
	int enclosing_block_begin(int position) const;
	int enclosing_block_end(int position) const;
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
	void PlanGprParam(const LowIRParam & param, int param_position,
	                  X64Register home_reg,
	                  std::vector<std::pair<int, X64Register> > & copies,
	                  std::vector<int> & crossing);
	void ClassifyGprParam(const ValueInfo & info, bool & late_operand_use,
	                      bool & address_use, bool & residue_exempt) const;
	void SpillParamHome(const LowIRParam & param, X64Register home_reg,
	                    const std::string & store_type);
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
	ValueLocation & resolve_location(const std::string & name);
	void RemoveDeadResultCopies();
	void emit_dest_copy(const std::string & dest, const LowIROperand & lhs,
	                    const LowIRType & type, bool normalize,
	                    X64Register & out_reg);
	void commit_dest(const std::string & dest);
	// Stores rax into `dest`'s already-allocated frame home (results
	// computed through the rax staging path when the destination has
	// no register).
	void commit_frame_result(const std::string & dest);

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

	// -- exception lowering (lowir_to_mir_flow.cpp). Functions with EH
	// ops compile in a conservative mode (eh_mode_): values keep frame
	// homes and never ride pool registers across instructions, because
	// the unwinder restores only rbp (plus the landing pad's own rsp
	// re-establishment) when it enters a landing pad, so register
	// state from before the throw is unreliable.
	void LowerException(const LowIRInstruction & ins);
	void LowerThrow(const LowIRInstruction & ins);
	void LowerResume(int position);

	// -- host-EH region dataflow (lowir_to_mir_eh.cpp)
	void AnalyzeEhRegions();
	void SimulateEhBlock(size_t block_index, std::vector<int> stack,
	                     std::vector<std::vector<int> > & in,
	                     std::vector<bool> & known,
	                     std::vector<size_t> & worklist);
	void MergeEhBlockState(const std::string & label,
	                       const std::vector<int> & stack,
	                       std::vector<std::vector<int> > & in,
	                       std::vector<bool> & known,
	                       std::vector<size_t> & worklist);
	int EhRegionForArming(int position, const LowIRInstruction & ins,
	                      int parent);
	std::vector<mir_model::HostEhClause> CollectEhClauses(
		const std::string & label) const;
	int eh_region_for(int position) const;
	void EmitEhLandingEntry();
	void AppendEhAbandonPads();
	void FinishEhRegions();

	// -- PA29 128-bit integers (lowir_to_mir_wide.cpp): frame-resident
	// pairs staged through rax:rdx.
	bool LowerWideInstruction(const LowIRInstruction & ins);
	long long WideHome(const std::string & name);
	void WideReadPair(const LowIROperand & op, X64Register lo,
	                  X64Register hi);
	void WideStorePair(long long home, X64Register lo, X64Register hi);
	void LowerWideBinary(const LowIRInstruction & ins);
	void LowerWideShift(const LowIRInstruction & ins);
	void LowerWideMul(const LowIRInstruction & ins);
	void LowerWideCmp(const LowIRInstruction & ins);
	void LowerWideConvert(const LowIRInstruction & ins);
	void LowerWideLoad(const LowIRInstruction & ins);
	void LowerWideStore(const LowIRInstruction & ins);

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
	bool eh_mode_ = false;   // function contains exception constructs
	bool gpr_read_staging_flip_ = false;   // r11/r10 spill-read rotation
	// host-EH region analysis results (lowir_to_mir_eh.cpp)
	std::vector<EhRegionPlan> eh_regions_;
	std::map<int, int> eh_region_of_position_;  // arming site -> region
	std::map<int, int> eh_region_at_;   // call site -> innermost region
	std::set<std::string> eh_landing_blocks_;
	std::map<std::string, size_t> block_index_of_;  // label -> block
	// PA29: known-constant i128 temps (shift counts arrive as widened
	// literals).
	std::map<std::string, long long> wide_consts_;

	// live state
	std::map<std::string, ValueLocation> locations_;
	std::string pool_holder_[kPoolSize];  // value name; "" free;
	                                      // "*pending:name" deferred load
	// register written since entry (excludes unmaterialized pending
	// reservations): a hoisted prologue param copy must not target one,
	// or code already emitted between the prologue and the hoist point
	// would clobber the copy (the pa31-audit double-booking bug)
	bool pool_clobbered_[kPoolSize];
	std::string xmm_holder_[8];
	std::string rax_alias_;               // value also present in rax
	long long frame_cursor_ = 0;
	long long residual_bytes_ = 0;
	int current_position_ = 0;
	std::map<std::string, const LowIRInstruction *> pending_loads_;
	bool index_dest_lowering_ = false;
	bool arg_homes_clobbered_ = false;
	bool has_dead_source_spill_ = false;
	long long pending_dest_spill_ = 0;   // frame offset; 0 = none
	size_t prologue_length_ = 0;
	std::vector<std::pair<int, int> > prologue_entries_;   // (param, home)
	size_t current_block_ = 0;
	mir_model::MirBlock * mir_block_ = 0;
};

}  // namespace lowir_to_mir
