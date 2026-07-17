#include "x86/lowir_to_mir.h"

#include <algorithm>
#include <stdexcept>

// Program- and function-scaffolding half of the LowIR -> MIR lowering:
// parameter ABI assignment and entry copies, frame finalization, the
// per-instruction dispatch, global conversion, and startup wiring.

namespace lowir_to_mir {

namespace {

// A forwarded parameter normally leaves an 8-byte residue in the frame
// (its unreclaimed home). The trivial promotion shape — the value flows
// through a promoted-slot copy and pure loads straight into the return —
// reclaims the residue entirely (200-trivial-param-slot-promotion).
bool forward_residue_reclaimed(
	const std::map<std::string, ValueInfo> & values,
	const std::vector<const LowIRInstruction *> & linear,
	const std::string & param)
{
	std::string current = param;
	bool through_copy = false;
	for(int guard = 0; guard < 64; guard++) {
		std::map<std::string, ValueInfo>::const_iterator it =
			values.find(current);
		if(it == values.end() || it->second.uses.size() != 1)
			return false;
		const ValueUse & use = it->second.uses[0];
		if(use.position < 0 || use.position >= (int)linear.size())
			return false;
		const LowIRInstruction & user = *linear[use.position];
		if(use.kind == ValueUse::USE_RETURN_VALUE)
			return through_copy;
		if(use.kind == ValueUse::USE_COPY_SOURCE) {
			if(user.result.empty())
				return false;
			through_copy = true;
			current = user.result;
			continue;
		}
		if(user.opcode == LOWIR_INS_LOAD && !user.result.empty()) {
			current = user.result;
			continue;
		}
		return false;
	}
	return false;
}

bool param_pass_annotated(const LowIRParam & param)
{
	return !param.metadata.find("pass").empty() &&
	       param.metadata.find("pass") != "direct";
}

}  // namespace

// Non-GPR parameters (xmm, stack-passed, f80) copy into named param-slot
// homes at entry; returns false when the parameter is GPR-class.
bool FunctionLowering::PlanWideParam(const LowIRParam & param,
                                     mir_model::ParamBinding & binding,
                                     int & xmm, int gpr,
                                     long long & stack_offset)
{
	bool is_xmm = param.type.kind == LOWIR_TYPE_F32 ||
	              param.type.kind == LOWIR_TYPE_F64;
	bool is_f80 = param.type.kind == LOWIR_TYPE_F80;
	bool is_wide = param.type.kind == LOWIR_TYPE_I128;
	bool big_obj = param.type.kind == LOWIR_TYPE_OBJ &&
	               param.type.obj_bytes > 8;
	if(is_xmm && xmm < 8) {
		binding.location = mir_model::ParamBinding::PL_XMM;
		binding.xmm = (XmmRegister)xmm;
		out_.params.push_back(binding);
		long long home = alloc_frame_home(
			param.name, param.type,
			mir_model::FrameBinding::FB_PARAM_SLOT);
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_FMOV);
		spill.type = SpellType(param.type);
		spill.operands.push_back(frame_operand(home));
		spill.operands.push_back(MakeXmm((XmmRegister)xmm));
		ValueLocation location;
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = home;
		locations_[param.name] = location;
		xmm++;
		return true;
	}
	if(!(is_f80 || is_wide || big_obj || (is_xmm && xmm >= 8) || gpr >= 6))
		return false;
	binding.location = mir_model::ParamBinding::PL_STACK;
	binding.stack_offset = stack_offset;
	out_.params.push_back(binding);
	LowIRType home_type = param.type;
	if(FrameSizeOf(home_type) < 8 && home_type.kind != LOWIR_TYPE_OBJ)
		home_type.kind = LOWIR_TYPE_I64;
	long long home = alloc_frame_home(
		param.name, home_type, mir_model::FrameBinding::FB_PARAM_SLOT);
	if(is_f80) {
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_FMOV);
		spill.type = "f80";
		spill.operands.push_back(frame_operand(home));
		spill.operands.push_back(frame_operand(stack_offset));
	}
	else {
		// copy the full padded container (one chunk for scalars and small
		// objects, several for by-value memory-class objects)
		invalidate_rax();
		long long copy_bytes =
			big_obj || is_wide ? FrameSizeOf(param.type) : 8;
		for(long long off = 0; off < copy_bytes; off += 8) {
			mir_model::Instruction & load =
				emit(mir_model::Instruction::MI_LOAD);
			load.type = "i64";
			load.operands.push_back(MakeReg(XR_RAX));
			load.operands.push_back(frame_operand(stack_offset + off));
			mir_model::Instruction & store =
				emit(mir_model::Instruction::MI_STORE);
			store.type = "i64";
			store.operands.push_back(frame_operand(home + off));
			store.operands.push_back(MakeReg(XR_RAX));
		}
	}
	ValueLocation location;
	location.kind = ValueLocation::VL_FRAME;
	location.frame_offset = home;
	locations_[param.name] = location;
	stack_offset += FrameSizeOf(param.type) < 8
		? 8 : (FrameSizeOf(param.type) + 7) / 8 * 8;
	return true;
}

void FunctionLowering::PlanParams()
{
	int gpr = 0, xmm = 0;
	long long stack_offset = 16;
	std::vector<std::pair<int, X64Register> > copies;
	std::vector<int> crossing;
	for(size_t p = 0; p < function_.params.size(); p++) {
		const LowIRParam & param = function_.params[p];
		mir_model::ParamBinding binding;
		binding.name = param.name;
		binding.type = SpellType(param.type);
		if(PlanWideParam(param, binding, xmm, gpr, stack_offset))
			continue;
		// GPR-class parameter
		X64Register home_reg = kArgRegs[gpr];
		binding.location = mir_model::ParamBinding::PL_REG;
		binding.reg = home_reg;
		if(param.type.kind == LOWIR_TYPE_OBJ)
			binding.type = ContainerSpelling(param.type.obj_bytes);
		out_.params.push_back(binding);
		gpr++;
		if(eh_mode_) {
			// EH functions keep every parameter in its frame home:
			// a register home would not survive unwinding back into
			// this frame.
			SpillParamHome(param, home_reg,
			               param.type.is_obj()
			                   ? "i64" : SpellType(param.type));
			continue;
		}
		PlanGprParam(param, (int)p, home_reg, copies, crossing);
	}
	// call-crossing parameters take callee-saved registers in reverse
	// declaration order
	for(size_t c = crossing.size(); c-- > 0; ) {
		const LowIRParam & param = function_.params[crossing[c]];
		int index = pool_scan(true, false);
		if(index < 0) {
			// no callee-saved register left: keep a named frame home
			SpillParamHome(param, out_.params[crossing[c]].reg,
			               SpellType(param.type));
			continue;
		}
		pool_holder_[index] = param.name;
		ValueLocation location;
		location.kind = ValueLocation::VL_GPR;
		location.reg = kPool[index];
		locations_[param.name] = location;
		copies.push_back(std::make_pair(crossing[c], kPool[index]));
	}
	std::sort(copies.begin(), copies.end());
	EmitGprParamCopies(copies);
	prologue_length_ = out_.blocks.empty()
		? 0 : out_.blocks[0].instructions.size();
	for(size_t c = 0; c < copies.size(); c++) {
		const mir_model::ParamBinding & binding =
			out_.params[copies[c].first];
		prologue_entries_.push_back(std::make_pair(
			copies[c].first, (int)binding.reg));
	}
}

// Classifies a register parameter's uses: late compare/binary operands
// take callee-saved registers; address-style uses scan from r8; single
// compare/branch/binary-operand/return uses leave no frame residue.
void FunctionLowering::ClassifyGprParam(const ValueInfo & info,
                                        bool & late_operand_use,
                                        bool & address_use,
                                        bool & residue_exempt) const
{
	int first_position = 0;
	while(skip_positions_.count(first_position))
		first_position++;
	for(size_t u = 0; u < info.uses.size(); u++) {
		if(info.uses[u].kind != ValueUse::USE_OTHER)
			continue;
		const LowIRInstruction & user = *linear_[info.uses[u].position];
		if((user.opcode == LOWIR_INS_BINARY ||
		    user.opcode == LOWIR_INS_CMP) &&
		   info.uses[u].position != first_position &&
		   info.uses.size() == 1)
			late_operand_use = true;
		if(user.opcode == LOWIR_INS_LOAD ||
		   user.opcode == LOWIR_INS_STORE ||
		   user.opcode == LOWIR_INS_COPYOBJ ||
		   user.opcode == LOWIR_INS_ZEROINIT ||
		   user.opcode == LOWIR_INS_ATOMIC_LOAD ||
		   user.opcode == LOWIR_INS_ATOMIC_STORE ||
		   user.opcode == LOWIR_INS_ATOMIC_EXCHANGE ||
		   user.opcode == LOWIR_INS_ATOMIC_COMPARE_EXCHANGE ||
		   user.opcode == LOWIR_INS_ATOMIC_ADD_FETCH)
			address_use = true;
	}
	if(info.uses.size() == 1) {
		const ValueUse & only = info.uses[0];
		const LowIRInstruction & user = *linear_[only.position];
		if(only.kind == ValueUse::USE_RETURN_VALUE)
			residue_exempt = true;
		if(user.opcode == LOWIR_INS_BINARY &&
		   only.kind == ValueUse::USE_OTHER)
			residue_exempt = true;
		if(user.opcode == LOWIR_INS_CMP ||
		   user.opcode == LOWIR_INS_BRANCH)
			residue_exempt = true;
	}
}

// Copies an incoming register parameter into a named frame home at entry.
void FunctionLowering::SpillParamHome(const LowIRParam & param,
                                      X64Register home_reg,
                                      const std::string & store_type)
{
	// The incoming register spills as a full 8-byte container, so the
	// home must span at least eight bytes even for narrow parameters.
	LowIRType home_type = param.type;
	if(FrameSizeOf(home_type) < 8) {
		if(home_type.kind == LOWIR_TYPE_OBJ) {
			home_type.obj_bytes = 8;
			home_type.obj_align = 8;
		}
		else {
			home_type.kind = LOWIR_TYPE_I64;
		}
	}
	long long home = alloc_frame_home(
		param.name, home_type, mir_model::FrameBinding::FB_PARAM_SLOT);
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = store_type;
	store.operands.push_back(frame_operand(home));
	store.operands.push_back(MakeReg(home_reg));
	ValueLocation location;
	location.kind = ValueLocation::VL_FRAME;
	location.frame_offset = home;
	locations_[param.name] = location;
}

void FunctionLowering::PlanGprParam(
	const LowIRParam & param, int param_position, X64Register home_reg,
	std::vector<std::pair<int, X64Register> > & copies,
	std::vector<int> & crossing)
{
	ValueInfo & info = values_[param.name];
	if(param.type.kind == LOWIR_TYPE_OBJ) {
		SpillParamHome(param, home_reg,
		               ContainerSpelling(param.type.obj_bytes));
		return;
	}
	if(info.uses.empty() && info.raw_references == 0) {
		// parameters the source never mentions still get their entry
		// spill into a named home
		has_dead_source_spill_ = true;
		SpillParamHome(param, home_reg, SpellType(param.type));
		return;
	}
	if(ParamUseIsForwardable(info, home_reg)) {
		ValueLocation location;
		location.kind = ValueLocation::VL_ARG_REG;
		location.reg = home_reg;
		locations_[param.name] = location;
		bool call_arg_use = false;
		for(size_t u = 0; u < info.uses.size(); u++)
			if(info.uses[u].kind == ValueUse::USE_CALL_ARG)
				call_arg_use = true;
		if(!call_arg_use && !param_pass_annotated(param) &&
		   !forward_residue_reclaimed(values_, linear_, param.name))
			residual_bytes_ += 8;
		return;
	}
	bool late_operand_use = false;
	bool address_use = false;
	bool residue_exempt = false;
	ClassifyGprParam(info, late_operand_use, address_use, residue_exempt);
	bool callee_saved = info.crosses_call || late_operand_use ||
	                    (address_use && info.uses.size() >= 2);
	if(!callee_saved && !residue_exempt && !param_pass_annotated(param))
		residual_bytes_ += 8;
	bool pool_home = home_reg == XR_R8 || home_reg == XR_R9;
	if(info.crosses_call && !late_operand_use) {
		crossing.push_back(param_position);
		return;
	}
	if(!callee_saved && !pool_home) {
		// scratch copies take their register at first read; address
		// bases and compare operands scan from r8, the rest from r9
		ValueLocation location;
		location.kind = ValueLocation::VL_PENDING_COPY;
		location.reg = home_reg;
		location.pending_r9_first = true;
		if(info.uses.size() == 1) {
			const LowIRInstruction & user =
				*linear_[info.uses[0].position];
			bool rhs_kind = info.uses[0].kind == ValueUse::USE_OTHER &&
				(user.opcode == LOWIR_INS_BINARY ||
				 user.opcode == LOWIR_INS_CMP);
			if(address_use && !rhs_kind)
				location.pending_r9_first = false;
			if(info.uses[0].kind == ValueUse::USE_BINARY_LHS &&
			   (user.opcode == LOWIR_INS_CMP ||
			    user.opcode == LOWIR_INS_BRANCH))
				location.pending_r9_first = false;
		}
		locations_[param.name] = location;
		return;
	}
	int index = pool_scan(callee_saved, false);
	if(index < 0) {
		// unplaceable parameters keep a named frame home at entry
		SpillParamHome(param, home_reg, SpellType(param.type));
		return;
	}
	pool_holder_[index] = param.name;
	ValueLocation location;
	location.kind = ValueLocation::VL_GPR;
	location.reg = kPool[index];
	locations_[param.name] = location;
	copies.push_back(std::make_pair(param_position, kPool[index]));
}

// Entry copies in parameter order, deferring any copy whose target
// register is the still-live incoming home of a later parameter.
void FunctionLowering::EmitGprParamCopies(
	const std::vector<std::pair<int, X64Register> > & copies)
{
	std::vector<std::pair<int, X64Register> > pending = copies;
	std::set<int> emitted;
	while(emitted.size() < pending.size()) {
		bool progress = false;
		for(size_t c = 0; c < pending.size(); c++) {
			if(emitted.count((int)c))
				continue;
			X64Register target = pending[c].second;
			bool conflict = false;
			for(size_t o = 0; o < pending.size(); o++) {
				if(o == c || emitted.count((int)o))
					continue;
				const mir_model::ParamBinding & other =
					out_.params[pending[o].first];
				if(other.location == mir_model::ParamBinding::PL_REG &&
				   other.reg == target)
					conflict = true;
			}
			if(conflict)
				continue;
			const mir_model::ParamBinding & binding =
				out_.params[pending[c].first];
			emit_mov(MakeReg(target), MakeReg(binding.reg));
			emitted.insert((int)c);
			progress = true;
		}
		if(!progress) {
			for(size_t c = 0; c < pending.size(); c++) {
				if(emitted.count((int)c))
					continue;
				const mir_model::ParamBinding & binding =
					out_.params[pending[c].first];
				emit_mov(MakeReg(pending[c].second), MakeReg(binding.reg));
				emitted.insert((int)c);
			}
		}
	}
}

// A pool-register copy of a call-style result is removed again when its
// target register is never mentioned later in the function (matching the
// reference's forward dead-copy sweep, which keeps a copy whose register
// is re-mentioned even only as a later write).
void FunctionLowering::RemoveDeadResultCopies()
{
	std::vector<mir_model::Instruction *> all;
	for(size_t b = 0; b < out_.blocks.size(); b++)
		for(size_t i = 0; i < out_.blocks[b].instructions.size(); i++)
			all.push_back(&out_.blocks[b].instructions[i]);
	std::set<X64Register> mentioned;
	std::set<mir_model::Instruction *> dead;
	for(size_t i = all.size(); i-- > 0; ) {
		mir_model::Instruction & ins = *all[i];
		if(ins.result_copy_hint &&
		   !mentioned.count(ins.operands[0].reg))
			dead.insert(&ins);
		// the original stream defines "mentioned later", so even removed
		// copies still count as a later mention for earlier candidates
		for(size_t o = 0; o < ins.operands.size(); o++) {
			if(ins.operands[o].kind == mir_model::Operand::OP_REG ||
			   ins.operands[o].kind == mir_model::Operand::OP_DEREF)
				mentioned.insert(ins.operands[o].reg);
		}
	}
	for(size_t b = 0; b < out_.blocks.size(); b++) {
		std::vector<mir_model::Instruction> kept;
		for(size_t i = 0; i < out_.blocks[b].instructions.size(); i++)
			if(!dead.count(&out_.blocks[b].instructions[i]))
				kept.push_back(out_.blocks[b].instructions[i]);
		out_.blocks[b].instructions.swap(kept);
	}
}

void FunctionLowering::FinishFrame()
{
	RemoveDeadResultCopies();
	// preserve exactly the callee-saved registers the final body mentions
	std::set<X64Register> used;
	for(size_t b = 0; b < out_.blocks.size(); b++)
		for(size_t i = 0; i < out_.blocks[b].instructions.size(); i++) {
			const mir_model::Instruction & ins =
				out_.blocks[b].instructions[i];
			for(size_t o = 0; o < ins.operands.size(); o++)
				if(ins.operands[o].kind == mir_model::Operand::OP_REG ||
				   ins.operands[o].kind == mir_model::Operand::OP_DEREF)
					used.insert(ins.operands[o].reg);
		}
	std::vector<X64Register> ordered;
	static const X64Register order[5] =
		{ XR_RBX, XR_R12, XR_R13, XR_R14, XR_R15 };
	for(int i = 0; i < 5; i++)
		if(used.count(order[i]))
			ordered.push_back(order[i]);
	out_.callee_saved_regs = ordered;
	if(has_dead_source_spill_ && residual_bytes_ == 0)
		residual_bytes_ = 8;
	long long scratch = touches_float_ ? 48 : 0;
	long long content = 8 * (long long)ordered.size() + frame_cursor_ +
	                    residual_bytes_;
	out_.stack_size = (size_t)((content + 15) / 16 * 16 + scratch);
	out_.scratch_bytes = (size_t)scratch;
	out_.frame_bytes = out_.stack_size;
}

void FunctionLowering::LowerBlock(size_t block_index)
{
	current_block_ = block_index;
	mir_block_ = &out_.blocks[block_index];
	// EH functions treat every block entry as a potential unwind
	// landing: nothing cached in rax (or lingering call staging) may
	// carry across the boundary.
	if(eh_mode_) {
		invalidate_rax();
		arg_homes_clobbered_ = true;
	}
	if(eh_landing_blocks_.count(function_.blocks[block_index].label))
		EmitEhLandingEntry();
	int begin = block_first_position_[block_index];
	int end = block_first_position_[block_index + 1];
	for(int p = begin; p < end; p++) {
		if(skip_positions_.count(p))
			continue;
		LowerInstruction(*linear_[p], p);
	}
}

void FunctionLowering::LowerInstruction(const LowIRInstruction & ins,
                                        int position)
{
	current_position_ = position;

	// PA29: 128-bit forms lower through the frame-resident pair path.
	// They stage pairs through rax:rdx with rcx/rsi/r10/r11 scratch, so
	// register-resident parameter homes do not survive them.
	if (LowerWideInstruction(ins))
	{
		arg_homes_clobbered_ = true;
		if (ins.result.empty() || !pending_loads_.count(ins.result))
			release_after_use(ins);
		return;
	}

	// Compares (and logical nots) consumed by their block's branch lower
	// as part of the branch; their own position emits nothing.
	if(!ins.result.empty() &&
	   (ins.opcode == LOWIR_INS_CMP ||
	    (ins.opcode == LOWIR_INS_UNARY && ins.operation == "not"))) {
		const ValueInfo & info = values_[ins.result];
		if(info.uses.size() == 1 &&
		   info.uses[0].position < (int)linear_.size()) {
			const LowIRInstruction & user = *linear_[info.uses[0].position];
			bool skip = false;
			bool invert = false;
			if(user.opcode == LOWIR_INS_BRANCH) {
				if(ins.opcode == LOWIR_INS_UNARY)
					skip = info.uses[0].position == position + 1;
				else
					skip = fused_compare_for_branch(
						user, info.uses[0].position, invert) == &ins;
			}
			else if(ins.opcode == LOWIR_INS_CMP &&
			        user.opcode == LOWIR_INS_UNARY &&
			        user.operation == "not" &&
			        info.uses[0].position == position + 1 &&
			        position + 2 < (int)linear_.size() &&
			        linear_[position + 2]->opcode == LOWIR_INS_BRANCH) {
				skip = fused_compare_for_branch(*linear_[position + 2],
				                                position + 2,
				                                invert) == &ins;
			}
			if(skip) {
				locations_[ins.result].kind = ValueLocation::VL_NONE;
				return;
			}
		}
	}

	switch(ins.opcode) {
		case LOWIR_INS_CALL:
		case LOWIR_INS_COPYOBJ:
		case LOWIR_INS_ZEROINIT:
		case LOWIR_INS_ATOMIC_LOAD:
		case LOWIR_INS_ATOMIC_STORE:
		case LOWIR_INS_ATOMIC_EXCHANGE:
		case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
		case LOWIR_INS_ATOMIC_ADD_FETCH:
			arg_homes_clobbered_ = true;
			break;
		default:
			break;
	}
	switch(ins.opcode) {
		case LOWIR_INS_CONST: LowerConst(ins); break;
		case LOWIR_INS_COPY: LowerCopy(ins); break;
		case LOWIR_INS_ADDR: LowerAddr(ins); break;
		case LOWIR_INS_LOAD: LowerLoad(ins); break;
		case LOWIR_INS_STORE: LowerStore(ins); break;
		case LOWIR_INS_INDEX: LowerIndex(ins); break;
		case LOWIR_INS_UNARY: LowerUnary(ins); break;
		case LOWIR_INS_BINARY: LowerBinary(ins); break;
		case LOWIR_INS_CMP: LowerCmpValue(ins); break;
		case LOWIR_INS_CONVERT: LowerConvert(ins); break;
		case LOWIR_INS_CALL: LowerCall(ins); break;
		case LOWIR_INS_COPYOBJ: LowerCopyObj(ins); break;
		case LOWIR_INS_ZEROINIT: LowerZeroInit(ins); break;
		case LOWIR_INS_ATOMIC_LOAD: LowerAtomicLoad(ins); break;
		case LOWIR_INS_ATOMIC_STORE: LowerAtomicStore(ins); break;
		case LOWIR_INS_ATOMIC_EXCHANGE: LowerAtomicExchange(ins); break;
		case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
			LowerAtomicCompareExchange(ins);
			break;
		case LOWIR_INS_ATOMIC_ADD_FETCH: LowerAtomicAddFetch(ins); break;
		case LOWIR_INS_ATOMIC_THREAD_FENCE:
			emit(mir_model::Instruction::MI_MFENCE);
			break;
		case LOWIR_INS_ATOMIC_SIGNAL_FENCE:
			break;   // compiler barrier only
		case LOWIR_INS_JUMP: LowerJump(ins); break;
		case LOWIR_INS_BRANCH: LowerBranch(ins); break;
		case LOWIR_INS_SWITCH: LowerSwitch(ins); break;
		case LOWIR_INS_RETURN: LowerReturn(ins); break;
		// Region arming/ending and classification markers are static
		// facts consumed by the host-EH region dataflow; they emit no
		// code of their own.
		case LOWIR_INS_EH_TRY: break;
		case LOWIR_INS_EH_CLEANUP: break;
		case LOWIR_INS_EH_END: break;
		case LOWIR_INS_EH_MARKER: break;
		case LOWIR_INS_EXCEPTION: LowerException(ins); break;
		case LOWIR_INS_THROW: LowerThrow(ins); break;
		case LOWIR_INS_RESUME: LowerResume(position); break;
		default:
			throw std::runtime_error(
				"PA28 does not lower exception constructs");
	}
	if(ins.result.empty() || !pending_loads_.count(ins.result))
		release_after_use(ins);
}

void FunctionLowering::Lower()
{
	Linearize();
	for(size_t p = 0; p < linear_.size() && !eh_mode_; p++) {
		switch(linear_[p]->opcode) {
			case LOWIR_INS_EH_TRY:
			case LOWIR_INS_EH_CLEANUP:
			case LOWIR_INS_EH_END:
			case LOWIR_INS_EH_MARKER:
			case LOWIR_INS_EXCEPTION:
			case LOWIR_INS_THROW:
			case LOWIR_INS_RESUME:
				eh_mode_ = true;
				break;
			default:
				break;
		}
	}
	// Unwinding re-enters dispatch blocks with only rbp/rsp restored,
	// so slot state must stay memory-resident in EH functions: the
	// slot types still register, but no slot promotes to registers.
	if(eh_mode_)
		for(size_t s = 0; s < function_.slots.size(); s++)
			slots_[function_.slots[s].name].type =
				function_.slots[s].type;
	else
		PromoteSlots();
	AnalyzeValues();
	MarkByAddressArgs();
	out_.blocks.resize(function_.blocks.size());
	for(size_t b = 0; b < function_.blocks.size(); b++)
		out_.blocks[b].label = function_.blocks[b].label;
	if(!out_.blocks.empty())
		mir_block_ = &out_.blocks[0];
	PlanParams();
	for(size_t s = 0; s < function_.slots.size(); s++) {
		SlotInfo & slot = slots_[function_.slots[s].name];
		if(slot.promoted)
			continue;
		if(!slot.alias_param.empty()) {
			slot.frame_offset = locations_[slot.alias_param].frame_offset;
			continue;
		}
		slot.frame_offset = alloc_frame_home(
			function_.slots[s].name, slot.type,
			mir_model::FrameBinding::FB_SLOT);
	}
	// Host-EH regions exist independently of eh_mode_: a function with
	// a plain `throw X(...)` carries a synthesized payload window but
	// no LowIR EH opcode.
	AnalyzeEhRegions();
	if(!eh_regions_.empty()) {
		out_.host_eh_enabled = true;
		LowIRType word;
		word.kind = LOWIR_TYPE_I64;
		out_.host_eh_exception_offset = alloc_frame_home(
			"__eh_exception", word, mir_model::FrameBinding::FB_TEMP);
		out_.host_eh_selector_offset = alloc_frame_home(
			"__eh_selector", word, mir_model::FrameBinding::FB_TEMP);
	}
	for(size_t b = 0; b < function_.blocks.size(); b++)
		LowerBlock(b);
	AppendEhAbandonPads();
	FinishEhRegions();
	FinishFrame();
}

}  // namespace lowir_to_mir

// -- program assembly ---------------------------------------------------

namespace {

using namespace lowir_to_mir;

std::string strip_sigil(const std::string & name)
{
	if(!name.empty() && name[0] == '@')
		return name.substr(1);
	return name;
}

mir_model::GlobalDefinition ConvertGlobal(const LowIRGlobal & global)
{
	mir_model::GlobalDefinition out;
	out.name = global.name;
	std::string storage = global.metadata.find("storage");
	out.readonly = storage == "readonly";
	out.thread_local_storage = storage == "thread_local";
	if(global.init == LOWIR_GLOBAL_STRUCTURED) {
		out.storage_kind = mir_model::GlobalDefinition::GS_DATA;
		for(size_t i = 0; i < global.items.size(); i++) {
			const LowIRDataItem & item = global.items[i];
			mir_model::GlobalDefinition::DataItem data;
			if(item.kind == LOWIR_DATA_ZERO) {
				data.kind = mir_model::GlobalDefinition::DataItem::ITEM_ZERO;
				data.zero_bytes = (size_t)item.zero_bytes;
			}
			else if(item.kind == LOWIR_DATA_ADDR) {
				data.kind = mir_model::GlobalDefinition::DataItem::ITEM_ADDR;
				data.symbol = item.symbol;
				data.addr_addend = item.addend;
			}
			else if(item.value.literal_class == LOWIR_LITERAL_INT ||
			        item.value.literal_class == LOWIR_LITERAL_NULLPTR) {
				data.kind =
					mir_model::GlobalDefinition::DataItem::ITEM_INTEGER;
				data.type = SpellType(item.type);
				data.int_value = ParseIntLiteral(item.value);
			}
			else {
				data.kind = mir_model::GlobalDefinition::DataItem::ITEM_FLOAT;
				data.type = SpellType(item.type);
				data.literal_text = (item.value.negated ? "-" : "") +
				                    item.value.literal;
				data.float_value = strtold(data.literal_text.c_str(), 0);
			}
			out.data_items.push_back(data);
		}
		return out;
	}
	out.storage_kind = mir_model::GlobalDefinition::GS_SCALAR;
	out.type = SpellType(global.type);
	switch(global.init) {
		case LOWIR_GLOBAL_ZERO:
			out.init_kind = mir_model::GlobalDefinition::GI_ZERO;
			break;
		case LOWIR_GLOBAL_SCALAR:
			if(global.scalar.literal_class == LOWIR_LITERAL_INT ||
			   global.scalar.literal_class == LOWIR_LITERAL_NULLPTR) {
				out.init_kind = mir_model::GlobalDefinition::GI_INTEGER;
				out.int_value = ParseIntLiteral(global.scalar);
			}
			else {
				out.init_kind = mir_model::GlobalDefinition::GI_FLOAT;
				out.literal_text = (global.scalar.negated ? "-" : "") +
				                   global.scalar.literal;
				out.float_value = strtold(out.literal_text.c_str(), 0);
			}
			break;
		case LOWIR_GLOBAL_ADDR:
			out.init_kind = mir_model::GlobalDefinition::GI_ADDR;
			out.symbol = global.addr_symbol;
			out.addr_addend = global.addr_addend;
			break;
		default:
			// declaration-only globals never reach here (the caller
			// filters on is_definition)
			throw std::logic_error("unsupported global initializer form");
	}
	return out;
}

void BuildStartup(const LowIRProgramInfo & info,
                  mir_model::MirProgram & program)
{
	if(info.entry_function.empty())
		return;
	mir_model::Instruction call;
	call.opcode = mir_model::Instruction::MI_CALL;
	if(!info.init_function.empty()) {
		call.operands.assign(1, MakeSymbol(info.init_function, false));
		program.startup.push_back(call);
	}
	call.operands.assign(1, MakeSymbol(info.entry_function, false));
	program.startup.push_back(call);
	mir_model::Instruction mov;
	mov.opcode = mir_model::Instruction::MI_MOV;
	if(!info.fini_function.empty()) {
		mov.operands.clear();
		mov.operands.push_back(MakeReg(XR_R12));
		mov.operands.push_back(MakeReg(XR_RAX));
		program.startup.push_back(mov);
		call.operands.assign(1, MakeSymbol(info.fini_function, false));
		program.startup.push_back(call);
		mov.operands.clear();
		mov.operands.push_back(MakeReg(XR_RDI));
		mov.operands.push_back(MakeReg(XR_R12));
		program.startup.push_back(mov);
	}
	else {
		mov.operands.clear();
		mov.operands.push_back(MakeReg(XR_RDI));
		mov.operands.push_back(MakeReg(XR_RAX));
		program.startup.push_back(mov);
	}
	mir_model::Instruction exit_ins;
	exit_ins.opcode = mir_model::Instruction::MI_EXIT;
	program.startup.push_back(exit_ins);
}

}  // namespace

mir_model::MirProgram LowerLowIRProgramToMir(const LowIRProgram & program,
                                             const LowIRProgramInfo & info,
                                             const std::string & target)
{
	mir_model::MirProgram result;
	result.target = target;
	ProgramFacts facts;
	facts.info = &info;
	for(size_t f = 0; f < program.functions.size(); f++) {
		const LowIRFunction & function = program.functions[f];
		std::string tls_for = function.metadata.find("tls_for");
		if(!tls_for.empty()) {
			std::string global = strip_sigil(tls_for);
			facts.tls_wrapper_of_global[global] = function.name;
			result.tls_wrappers[function.name] = global;
		}
	}
	for(size_t g = 0; g < program.globals.size(); g++) {
		if(!program.globals[g].is_definition)
			continue;
		result.globals.push_back(ConvertGlobal(program.globals[g]));
	}
	for(size_t f = 0; f < program.functions.size(); f++) {
		const LowIRFunction & function = program.functions[f];
		if(!function.is_definition)
			continue;
		mir_model::MirFunction lowered;
		lowered.name = function.name;
		lowered.return_type = SpellType(function.return_type);
		FunctionLowering lowering(function, facts, lowered);
		lowering.Lower();
		result.functions.push_back(lowered);
	}
	BuildStartup(info, result);
	return result;
}
