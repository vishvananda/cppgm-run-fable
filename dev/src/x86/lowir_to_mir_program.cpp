#include "x86/lowir_to_mir.h"

#include <algorithm>
#include <stdexcept>

// Program- and function-scaffolding half of the LowIR -> MIR lowering:
// parameter ABI assignment and entry copies, frame finalization, the
// per-instruction dispatch, global conversion, and startup wiring.

namespace lowir_to_mir {

namespace {

const X64Register kArgRegs[6] =
	{ XR_RDI, XR_RSI, XR_RDX, XR_RCX, XR_R8, XR_R9 };

X64Register pool_reg_at(int index)
{
	static const X64Register pool[FunctionLowering::kPoolSize] =
		{ XR_R8, XR_R9, XR_RBX, XR_R12, XR_R13, XR_R14, XR_R15 };
	return pool[index];
}

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

bool param_expects_address(const LowIRParam & param)
{
	if(param.type.kind != LOWIR_TYPE_PTR)
		return false;
	std::string pass = param.metadata.find("pass");
	return pass == "indirect_result" || pass == "by_address" ||
	       pass == "reference";
}

std::string container_spelling(long long bytes)
{
	if(bytes <= 1) return "i8";
	if(bytes <= 2) return "i16";
	if(bytes <= 4) return "i32";
	return "i64";
}

}  // namespace

// Marks temps whose address must be materialized for a by-address call
// argument; they get frame homes at definition.
static void MarkByAddressArgs(const std::vector<const LowIRInstruction *> & linear,
                              const ProgramFacts & facts,
                              std::map<std::string, ValueInfo> & values)
{
	for(size_t p = 0; p < linear.size(); p++) {
		const LowIRInstruction & ins = *linear[p];
		if(ins.opcode != LOWIR_INS_CALL)
			continue;
		const std::vector<LowIRParam> * params = 0;
		if(!ins.callee_is_temp && facts.info->is_function(ins.callee))
			params = &facts.info->functions.find(ins.callee)->second->params;
		else if(ins.signature.present)
			params = &ins.signature.params;
		if(!params)
			continue;
		for(size_t a = 0; a < ins.operands.size() && a < params->size(); a++) {
			const LowIROperand & arg = ins.operands[a];
			if(arg.kind != LOWIR_OPERAND_TEMP)
				continue;
			if(!param_expects_address((*params)[a]))
				continue;
			std::map<std::string, ValueInfo>::iterator it =
				values.find(arg.name);
			if(it != values.end() &&
			   it->second.type.kind != LOWIR_TYPE_PTR)
				it->second.needs_frame = true;
		}
	}
}

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
	if(!(is_f80 || big_obj || (is_xmm && xmm >= 8) || gpr >= 6))
		return false;
	binding.location = mir_model::ParamBinding::PL_STACK;
	binding.stack_offset = stack_offset;
	out_.params.push_back(binding);
	long long home = alloc_frame_home(
		param.name, param.type, mir_model::FrameBinding::FB_PARAM_SLOT);
	if(is_f80) {
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_FMOV);
		spill.type = "f80";
		spill.operands.push_back(frame_operand(home));
		spill.operands.push_back(frame_operand(stack_offset));
	}
	else {
		invalidate_rax();
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = "i64";
		load.operands.push_back(MakeReg(XR_RAX));
		load.operands.push_back(frame_operand(stack_offset));
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = "i64";
		store.operands.push_back(frame_operand(home));
		store.operands.push_back(MakeReg(XR_RAX));
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
	for(size_t p = 0; p < function_.params.size(); p++) {
		const LowIRParam & param = function_.params[p];
		ValueInfo & info = values_[param.name];
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
			binding.type = container_spelling(param.type.obj_bytes);
		out_.params.push_back(binding);
		gpr++;
		if(param.type.kind == LOWIR_TYPE_OBJ) {
			long long home = alloc_frame_home(
				param.name, param.type,
				mir_model::FrameBinding::FB_PARAM_SLOT);
			mir_model::Instruction & store =
				emit(mir_model::Instruction::MI_STORE);
			store.type = container_spelling(param.type.obj_bytes);
			store.operands.push_back(frame_operand(home));
			store.operands.push_back(MakeReg(home_reg));
			ValueLocation location;
			location.kind = ValueLocation::VL_FRAME;
			location.frame_offset = home;
			locations_[param.name] = location;
			continue;
		}
		if(info.uses.empty() && info.raw_references == 0) {
			// parameters the source never mentions still get their
			// entry spill into a named home
			has_dead_source_spill_ = true;
			long long home = alloc_frame_home(
				param.name, param.type,
				mir_model::FrameBinding::FB_PARAM_SLOT);
			mir_model::Instruction & store =
				emit(mir_model::Instruction::MI_STORE);
			store.type = SpellType(param.type);
			store.operands.push_back(frame_operand(home));
			store.operands.push_back(MakeReg(home_reg));
			ValueLocation location;
			location.kind = ValueLocation::VL_FRAME;
			location.frame_offset = home;
			locations_[param.name] = location;
			continue;
		}
		if(ParamUseIsForwardable(info, home_reg)) {
			int index = pool_scan(false, false);
			if(index >= 0)
				pool_holder_[index] = "*hole:" + param.name;
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
			continue;
		}
		bool late_operand_use = false;
		bool address_use = false;
		int first_position = 0;
		while(skip_positions_.count(first_position))
			first_position++;
		for(size_t u = 0; u < info.uses.size(); u++) {
			if(info.uses[u].kind != ValueUse::USE_OTHER)
				continue;
			const LowIRInstruction & user =
				*linear_[info.uses[u].position];
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
		bool callee_saved = info.crosses_call || late_operand_use ||
		                    (address_use && info.uses.size() >= 2);
		int index = pool_scan(callee_saved, !callee_saved && !address_use);
		if(index < 0) {
			// unplaceable parameters keep a named frame home at entry
			long long home = alloc_frame_home(
				param.name, param.type,
				mir_model::FrameBinding::FB_PARAM_SLOT);
			mir_model::Instruction & store =
				emit(mir_model::Instruction::MI_STORE);
			store.type = SpellType(param.type);
			store.operands.push_back(frame_operand(home));
			store.operands.push_back(MakeReg(home_reg));
			ValueLocation location;
			location.kind = ValueLocation::VL_FRAME;
			location.frame_offset = home;
			locations_[param.name] = location;
			continue;
		}
		pool_holder_[index] = param.name;
		if(index >= 2)
			note_callee_saved(pool_reg_at(index));
		ValueLocation location;
		location.kind = ValueLocation::VL_GPR;
		location.reg = pool_reg_at(index);
		location.prefer_home = !callee_saved && !address_use;
		locations_[param.name] = location;
		copies.push_back(std::make_pair((int)p, pool_reg_at(index)));
		bool rhs_only = info.uses.size() == 1 &&
		                info.uses[0].kind == ValueUse::USE_OTHER &&
		                (linear_[info.uses[0].position]->opcode ==
		                     LOWIR_INS_BINARY ||
		                 linear_[info.uses[0].position]->opcode ==
		                     LOWIR_INS_CMP);
		if(!callee_saved && !rhs_only && !param_pass_annotated(param))
			residual_bytes_ += 8;
	}
	EmitGprParamCopies(copies);
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
		default:
			throw std::runtime_error(
				"PA28 does not lower exception constructs");
	}
	if(pending_load_ != linear_[position])
		release_after_use(ins);
}

void FunctionLowering::Lower()
{
	Linearize();
	PromoteSlots();
	AnalyzeValues();
	MarkByAddressArgs(linear_, facts_, values_);
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
	for(size_t b = 0; b < function_.blocks.size(); b++)
		LowerBlock(b);
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
			out.init_kind = mir_model::GlobalDefinition::GI_ZERO;
			break;
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
