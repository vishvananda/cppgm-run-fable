#include "x86/lowir_to_mir.h"

#include <stdexcept>

// Calls, bulk memory, atomics, and control flow for the LowIR -> MIR
// lowering. Argument staging, indirect-target staging through r10, and
// the atomic instruction sequences follow the pa28 strict fixtures.

namespace lowir_to_mir {

// Splits call arguments into GPR / XMM / stack slots in argument order.
// This is the single owner of the caller-side argument classification;
// CallArgTargetsHome consumes the same slots when it decides forwarding.
std::vector<ArgSlot> ClassifyCallArgs(
	const std::vector<LowIRParam> & params, const LowIRInstruction & ins,
	const std::map<std::string, ValueInfo> & values)
{
	std::vector<ArgSlot> slots;
	int gpr = 0, xmm = 0;
	long long stack = 0;
	for(size_t a = 0; a < ins.operands.size(); a++) {
		ArgSlot slot;
		if(a < params.size()) {
			slot.param_type = params[a].type;
			slot.by_address = ParamPassWantsAddress(params[a]);
		}
		else {
			// variadic tail: classify by the argument's own value
			const LowIROperand & arg = ins.operands[a];
			if(arg.kind == LOWIR_OPERAND_LITERAL) {
				slot.param_type.kind =
					arg.literal_class == LOWIR_LITERAL_F32 ? LOWIR_TYPE_F32
					: arg.literal_class == LOWIR_LITERAL_F64 ? LOWIR_TYPE_F64
					: arg.literal_class == LOWIR_LITERAL_F80 ? LOWIR_TYPE_F80
					: LOWIR_TYPE_I64;
			}
			else if(arg.kind == LOWIR_OPERAND_TEMP &&
			        values.count(arg.name)) {
				slot.param_type = values.find(arg.name)->second.type;
			}
			else {
				slot.param_type.kind = LOWIR_TYPE_PTR;
			}
		}
		const LowIRType & type = slot.param_type;
		if(type.kind == LOWIR_TYPE_F32 || type.kind == LOWIR_TYPE_F64) {
			if(xmm < 8) {
				slot.kind = ArgSlot::AS_XMM;
				slot.ordinal = xmm++;
			}
			else {
				slot.kind = ArgSlot::AS_STACK;
				slot.stack_offset = stack;
				stack += 8;
			}
		}
		else if(type.kind == LOWIR_TYPE_F80) {
			slot.kind = ArgSlot::AS_STACK;
			slot.stack_offset = stack;
			slot.stack_bytes = 16;
			stack += slot.stack_bytes;
		}
		else if(type.kind == LOWIR_TYPE_OBJ && type.obj_bytes > 8) {
			slot.kind = ArgSlot::AS_STACK;
			slot.stack_offset = stack;
			slot.stack_bytes = (type.obj_bytes + 7) / 8 * 8;
			stack += slot.stack_bytes;
		}
		else if(gpr < 6) {
			slot.kind = ArgSlot::AS_GPR;
			slot.ordinal = gpr++;
		}
		else {
			slot.kind = ArgSlot::AS_STACK;
			slot.stack_offset = stack;
			stack += slot.stack_bytes;
		}
		slots.push_back(slot);
	}
	return slots;
}

namespace {

long long stack_bytes_of(const std::vector<ArgSlot> & slots)
{
	long long total = 0;
	for(size_t i = 0; i < slots.size(); i++)
		if(slots[i].kind == ArgSlot::AS_STACK &&
		   slots[i].stack_offset + slots[i].stack_bytes > total)
			total = slots[i].stack_offset + slots[i].stack_bytes;
	return (total + 15) / 16 * 16;
}

}  // namespace

void FunctionLowering::LowerCall(const LowIRInstruction & ins)
{
	bool indirect = ins.callee_is_temp;
	const std::vector<LowIRParam> * params = 0;
	LowIRType return_type;
	if(!indirect && facts_.info->is_function(ins.callee)) {
		const LowIRFunction * callee =
			facts_.info->functions.find(ins.callee)->second;
		params = &callee->params;
		return_type = callee->return_type;
	}
	else if(ins.signature.present) {
		indirect = true;
		params = &ins.signature.params;
		return_type = ins.signature.return_type;
	}
	else {
		throw std::runtime_error("call to unknown callee @" + ins.callee);
	}
	std::vector<ArgSlot> slots = ClassifyCallArgs(*params, ins, values_);
	long long stack_adjust = stack_bytes_of(slots);
	int gpr_count = 0;
	for(size_t i = 0; i < slots.size(); i++)
		if(slots[i].kind == ArgSlot::AS_GPR)
			gpr_count++;

	// Integer results claim their landing spot while the arguments are
	// still live, so high-pressure call sites keep the fixture shape of a
	// frame temp reserved ahead of the argument spills.
	enum { RP_NONE, RP_COLLAPSE, RP_POOL, RP_FRAME } result_plan = RP_NONE;
	X64Register result_reg = XR_RAX;
	long long result_offset = 0;
	if(!ins.result.empty() &&
	   return_type.kind != LOWIR_TYPE_F32 &&
	   return_type.kind != LOWIR_TYPE_F64 &&
	   return_type.kind != LOWIR_TYPE_F80 &&
	   return_type.kind != LOWIR_TYPE_OBJ &&
	   !return_type.is_void()) {
		const ValueInfo & result = values_[ins.result];
		if(result.uses.empty()) {
			result_plan = RP_NONE;
		}
		else if(result.uses.size() == 1 && !result.needs_frame &&
		        result.uses[0].position == current_position_ + 1 &&
		        result.uses[0].kind == ValueUse::USE_RETURN_VALUE) {
			result_plan = RP_COLLAPSE;
		}
		else {
			LowIRType home_type = return_type;
			if(FrameSizeOf(home_type) < 8 &&
			   home_type.kind != LOWIR_TYPE_OBJ)
				home_type.kind = LOWIR_TYPE_I64;
			if(result.needs_frame) {
				result_plan = RP_FRAME;
				result_offset = alloc_frame_home(
					ins.result, home_type,
					mir_model::FrameBinding::FB_TEMP);
			}
			else if(alloc_pool_gpr(ins.result, result_reg)) {
				result_plan = RP_POOL;
			}
			else {
				result_plan = RP_FRAME;
				result_offset = alloc_frame_home(
					ins.result, home_type,
					mir_model::FrameBinding::FB_TEMP);
			}
		}
	}

	// Forwarded parameters still riding their incoming argument register
	// bounce through a free scratch register before the staging writes.
	for(size_t a = 0; a < ins.operands.size(); a++) {
		const LowIROperand & arg = ins.operands[a];
		if(arg.kind != LOWIR_OPERAND_TEMP)
			continue;
		ValueLocation & bounce = locations_[arg.name];
		if(bounce.kind != ValueLocation::VL_ARG_REG)
			continue;
		int index = pool_scan(false, false);
		if(index < 0) {
			// no scratch register free: park the value in the frame
			long long offset = alloc_anonymous_spill();
			mir_model::Instruction & spill =
				emit(mir_model::Instruction::MI_STORE);
			spill.type = "i64";
			spill.operands.push_back(frame_operand(offset));
			spill.operands.push_back(MakeReg(bounce.reg));
			bounce.kind = ValueLocation::VL_FRAME;
			bounce.frame_offset = offset;
			continue;
		}
		X64Register scratch = kPool[index];
		emit_mov(MakeReg(scratch), MakeReg(bounce.reg));
		pool_holder_[index] = arg.name;
		bounce.kind = ValueLocation::VL_GPR;
		bounce.reg = scratch;
	}

	// Argument sources living in the pool registers that double as the
	// fifth and sixth argument registers spill to anonymous frame slots.
	for(size_t a = 0; a < ins.operands.size(); a++) {
		const LowIROperand & arg = ins.operands[a];
		if(arg.kind != LOWIR_OPERAND_TEMP)
			continue;
		ValueLocation & location = locations_[arg.name];
		if(location.kind != ValueLocation::VL_GPR)
			continue;
		bool endangered =
			(location.reg == XR_R8 && gpr_count >= 5) ||
			(location.reg == XR_R9 && gpr_count >= 6);
		if(!endangered)
			continue;
		long long offset = alloc_anonymous_spill();
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_STORE);
		spill.type = "i64";
		spill.operands.push_back(frame_operand(offset));
		spill.operands.push_back(MakeReg(location.reg));
		release_value(arg.name);
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = offset;
		locations_[arg.name] = location;
	}

	// Argument sources living in the low xmm registers that the staging
	// itself writes spill to anonymous frame slots first. A source is safe
	// when its register is its own slot (that write is skipped) or when
	// every read of it happens before its register is overwritten.
	int xmm_count = 0;
	for(size_t i = 0; i < slots.size(); i++)
		if(slots[i].kind == ArgSlot::AS_XMM)
			xmm_count++;
	for(size_t a = 0; a < ins.operands.size(); a++) {
		const LowIROperand & arg = ins.operands[a];
		if(arg.kind != LOWIR_OPERAND_TEMP)
			continue;
		ValueLocation & location = locations_[arg.name];
		if(location.kind != ValueLocation::VL_XMM ||
		   (int)location.xmm >= xmm_count)
			continue;
		bool in_place = false, read_after_write = false;
		for(size_t b = 0; b < ins.operands.size(); b++) {
			if(ins.operands[b].kind != LOWIR_OPERAND_TEMP ||
			   ins.operands[b].name != arg.name)
				continue;
			if(slots[b].kind == ArgSlot::AS_XMM &&
			   slots[b].ordinal == (int)location.xmm)
				in_place = true;
			else if(slots[b].kind != ArgSlot::AS_XMM ||
			        slots[b].ordinal > (int)location.xmm)
				read_after_write = true;
		}
		if(in_place || !read_after_write)
			continue;
		const LowIRType & value_type = values_[arg.name].type;
		long long offset = alloc_anonymous_spill();
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_FMOV);
		spill.type = SpellType(value_type);
		spill.operands.push_back(frame_operand(offset));
		spill.operands.push_back(MakeXmm(location.xmm));
		release_value(arg.name);
		ValueLocation & spot = locations_[arg.name];
		spot.kind = ValueLocation::VL_FRAME;
		spot.frame_offset = offset;
	}

	if(stack_adjust > 0) {
		mir_model::Instruction & sub = emit(mir_model::Instruction::MI_SUB);
		sub.operands.push_back(MakeReg(XR_RSP));
		sub.operands.push_back(MakeImm(stack_adjust));
	}

	if(indirect) {
		LowIROperand callee;
		callee.kind = ins.callee_is_temp ? LOWIR_OPERAND_TEMP
		                                 : LOWIR_OPERAND_GLOBAL;
		callee.name = ins.callee;
		if(!ins.callee_is_temp) {
			// pointer-valued global cell: call through the stored pointer
			mir_model::Instruction & load =
				emit(mir_model::Instruction::MI_LOAD);
			load.type = "ptr";
			load.operands.push_back(MakeReg(XR_R10));
			load.operands.push_back(MakeSymbol(ins.callee, true));
		}
		else {
			const ValueInfo & info = values_[ins.callee];
			const LowIRInstruction * def =
				info.def_position >= 0 &&
				info.def_position < (int)linear_.size()
					? linear_[info.def_position] : 0;
			const ValueLocation & location = locations_[ins.callee];
			if(def && def->opcode == LOWIR_INS_ADDR &&
			   def->operands[0].kind == LOWIR_OPERAND_GLOBAL &&
			   !facts_.info->is_function(def->operands[0].name) &&
			   !storage_is_tls(def->operands[0])) {
				// pointer stored in the named cell, not the cell address
				mir_model::Instruction & load =
					emit(mir_model::Instruction::MI_LOAD);
				load.type = "ptr";
				load.operands.push_back(MakeReg(XR_R10));
				load.operands.push_back(
					MakeSymbol(def->operands[0].name, true));
			}
			else if(location.kind == ValueLocation::VL_FRAME) {
				mir_model::Instruction & load =
					emit(mir_model::Instruction::MI_LOAD);
				load.type = "ptr";
				load.operands.push_back(MakeReg(XR_R10));
				load.operands.push_back(
					frame_operand(location.frame_offset));
			}
			else {
				emit_mov(MakeReg(XR_R10), MakeReg(location.reg));
			}
		}
	}

	for(int phase = 0; phase < 3; phase++)
	for(size_t a = 0; a < ins.operands.size(); a++) {
		const ArgSlot & slot = slots[a];
		const LowIROperand & arg = ins.operands[a];
		int slot_phase = slot.kind == ArgSlot::AS_GPR ? 0
			: slot.kind == ArgSlot::AS_XMM ? 1 : 2;
		if(slot_phase != phase)
			continue;
		if(slot.kind == ArgSlot::AS_GPR) {
			X64Register target = kArgRegs[slot.ordinal];
			if(arg.kind == LOWIR_OPERAND_LITERAL) {
				emit_mov(MakeReg(target), MakeImm(ParseIntLiteral(arg)));
			}
			else if(arg.kind == LOWIR_OPERAND_SLOT) {
				const SlotInfo & home = slots_[arg.name];
				bool by_address = slot.by_address ||
					(slot.param_type.kind == LOWIR_TYPE_PTR &&
					 home.type.kind != LOWIR_TYPE_PTR) ||
					home.type.kind == LOWIR_TYPE_OBJ;
				if(by_address) {
					mir_model::Instruction & lea =
						emit(mir_model::Instruction::MI_LEA);
					lea.operands.push_back(MakeReg(target));
					lea.operands.push_back(
						frame_operand(home.frame_offset));
				}
				else {
					mir_model::Instruction & fill =
						emit(mir_model::Instruction::MI_LOAD);
					fill.type = SpellType(home.type);
					fill.operands.push_back(MakeReg(target));
					fill.operands.push_back(
						frame_operand(home.frame_offset));
				}
			}
			else if(arg.kind == LOWIR_OPERAND_GLOBAL) {
				emit_mov(MakeReg(target),
				         MakeSymbol(arg.name,
				                    !facts_.info->is_function(arg.name)));
			}
			else if(operand_is_pending(arg)) {
				const LowIRInstruction * load = take_pending(arg);
				mir_model::Operand address =
					address_operand(load->operands[0], target);
				release_after_use(*load);
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_LOAD);
				fill.type = SpellType(load->type);
				fill.operands.push_back(MakeReg(target));
				fill.operands.push_back(address);
			}
			else {
				const ValueLocation & location = resolve_location(arg.name);
				const ValueInfo & info = values_[arg.name];
				if(slot.by_address && info.type.kind != LOWIR_TYPE_PTR) {
					long long offset = location.frame_offset;
					if(location.kind != ValueLocation::VL_FRAME) {
						// materialize the value into a frame home now
						offset = alloc_anonymous_spill();
						mir_model::Instruction & store =
							emit(mir_model::Instruction::MI_STORE);
						store.type = SpellType(info.type);
						store.operands.push_back(frame_operand(offset));
						store.operands.push_back(
							MakeReg(location.reg));
						ValueLocation & spot = locations_[arg.name];
						spot.kind = ValueLocation::VL_FRAME;
						spot.frame_offset = offset;
					}
					mir_model::Instruction & lea =
						emit(mir_model::Instruction::MI_LEA);
					lea.operands.push_back(MakeReg(target));
					lea.operands.push_back(frame_operand(offset));
				}
				else if(location.kind == ValueLocation::VL_SLOT_ADDR) {
					mir_model::Instruction & lea =
						emit(mir_model::Instruction::MI_LEA);
					lea.operands.push_back(MakeReg(target));
					lea.operands.push_back(frame_operand(
						slots_[location.slot_name].frame_offset));
				}
				else if(location.kind == ValueLocation::VL_FRAME &&
				        info.type.kind == LOWIR_TYPE_OBJ) {
					mir_model::Instruction & lea =
						emit(mir_model::Instruction::MI_LEA);
					lea.operands.push_back(MakeReg(XR_R11));
					lea.operands.push_back(
						frame_operand(location.frame_offset));
					mir_model::Instruction & load =
						emit(mir_model::Instruction::MI_LOAD);
					load.type = ContainerSpelling(info.type.obj_bytes);
					load.operands.push_back(MakeReg(target));
					load.operands.push_back(MakeDeref(XR_R11, 0));
				}
				else if(location.kind == ValueLocation::VL_FRAME) {
					mir_model::Instruction & load =
						emit(mir_model::Instruction::MI_LOAD);
					load.type = SpellType(info.type);
					load.operands.push_back(MakeReg(target));
					load.operands.push_back(
						frame_operand(location.frame_offset));
				}
				else {
					emit_mov(MakeReg(target), MakeReg(location.reg));
				}
			}
		}
		else if(slot.kind == ArgSlot::AS_XMM) {
			mir_model::Operand source = float_read(arg, slot.param_type);
			if(source.kind == mir_model::Operand::OP_XMM &&
			   source.xmm == (XmmRegister)slot.ordinal)
				continue;   // already staged in place
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = SpellType(slot.param_type);
			mov.operands.push_back(MakeXmm((XmmRegister)slot.ordinal));
			mov.operands.push_back(source);
		}
		else if(slot.param_type.kind == LOWIR_TYPE_F80) {
			mir_model::Operand source = float_read(arg, slot.param_type);
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = "f80";
			mov.operands.push_back(MakeDeref(XR_RSP, slot.stack_offset));
			mov.operands.push_back(source);
		}
		else if(slot.param_type.kind == LOWIR_TYPE_OBJ &&
		        slot.param_type.obj_bytes > 8) {
			// by-value memory-class object: copy the padded container into
			// the reserved stack region in 8-byte chunks through r11
			mir_model::Operand base;
			if(arg.kind == LOWIR_OPERAND_SLOT)
				base = frame_operand(slots_[arg.name].frame_offset);
			else if(arg.kind == LOWIR_OPERAND_TEMP &&
			        resolve_location(arg.name).kind ==
			            ValueLocation::VL_FRAME)
				base = frame_operand(
					locations_[arg.name].frame_offset);
			else
				throw std::runtime_error(
					"unsupported by-value object argument");
			for(long long off = 0; off < slot.stack_bytes; off += 8) {
				mir_model::Operand chunk = base;
				chunk.offset += off;
				mir_model::Instruction & load =
					emit(mir_model::Instruction::MI_LOAD);
				load.type = "i64";
				load.operands.push_back(MakeReg(XR_R11));
				load.operands.push_back(chunk);
				mir_model::Instruction & store =
					emit(mir_model::Instruction::MI_STORE);
				store.type = "i64";
				store.operands.push_back(
					MakeDeref(XR_RSP, slot.stack_offset + off));
				store.operands.push_back(MakeReg(XR_R11));
			}
		}
		else {
			// integer-class stack argument staged through r11
			mir_model::Operand source;
			if(arg.kind == LOWIR_OPERAND_LITERAL) {
				emit_mov(MakeReg(XR_R11), MakeImm(ParseIntLiteral(arg)));
				source = MakeReg(XR_R11);
			}
			else if(arg.kind == LOWIR_OPERAND_SLOT) {
				mir_model::Instruction & lea =
					emit(mir_model::Instruction::MI_LEA);
				lea.operands.push_back(MakeReg(XR_R11));
				lea.operands.push_back(
					frame_operand(slots_[arg.name].frame_offset));
				source = MakeReg(XR_R11);
			}
			else if(operand_is_pending(arg)) {
				// a deferred single-use load lands directly in r11
				const LowIRInstruction * load = take_pending(arg);
				mir_model::Operand address =
					address_operand(load->operands[0], XR_R11);
				release_after_use(*load);
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_LOAD);
				fill.type = SpellType(load->type);
				fill.operands.push_back(MakeReg(XR_R11));
				fill.operands.push_back(address);
				source = MakeReg(XR_R11);
			}
			else {
				const ValueLocation & location = resolve_location(arg.name);
				if(location.kind == ValueLocation::VL_FRAME) {
					mir_model::Instruction & load =
						emit(mir_model::Instruction::MI_LOAD);
					load.type = "i64";
					load.operands.push_back(MakeReg(XR_R11));
					load.operands.push_back(
						frame_operand(location.frame_offset));
					source = MakeReg(XR_R11);
				}
				else {
					source = MakeReg(location.reg);
				}
			}
			mir_model::Instruction & store =
				emit(mir_model::Instruction::MI_STORE);
			store.type = "i64";
			store.operands.push_back(MakeDeref(XR_RSP, slot.stack_offset));
			store.operands.push_back(source);
		}
	}

	if(indirect) {
		mir_model::Instruction & call =
			emit(mir_model::Instruction::MI_CALL_INDIRECT);
		call.operands.push_back(MakeReg(XR_R10));
	}
	else {
		mir_model::Instruction & call = emit(mir_model::Instruction::MI_CALL);
		call.operands.push_back(MakeSymbol(ins.callee, false));
	}

	if(stack_adjust > 0) {
		mir_model::Instruction & add = emit(mir_model::Instruction::MI_ADD);
		add.operands.push_back(MakeReg(XR_RSP));
		add.operands.push_back(MakeImm(stack_adjust));
	}

	invalidate_rax();

	if(ins.result.empty())
		return;
	const ValueInfo & result = values_[ins.result];
	if(result.uses.empty())
		return;   // dead call result: no store, no copy
	if(return_type.kind == LOWIR_TYPE_F80) {
		long long offset = f80_result_home(ins.result);
		mir_model::Instruction & fstp =
			emit(mir_model::Instruction::MI_FSTP);
		fstp.type = "f80";
		fstp.operands.push_back(frame_operand(offset));
		return;
	}
	if(return_type.kind == LOWIR_TYPE_F32 ||
	   return_type.kind == LOWIR_TYPE_F64) {
		if(result.crosses_call || result.needs_frame) {
			long long offset = f80_result_home(ins.result);
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = SpellType(return_type);
			mov.operands.push_back(frame_operand(offset));
			mov.operands.push_back(MakeXmm(XMM_0));
			return;
		}
		XmmRegister xmm = alloc_xmm(ins.result);
		mir_model::Instruction & mov = emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(return_type);
		mov.operands.push_back(MakeXmm(xmm));
		mov.operands.push_back(MakeXmm(XMM_0));
		return;
	}
	if(return_type.kind == LOWIR_TYPE_OBJ) {
		// A single adjacent copyobj consumer reads the value straight from
		// rax; otherwise the object value lands in its own frame home.
		bool foldable_copy = result.uses.size() == 1 &&
			result.uses[0].position > current_position_ &&
			result.uses[0].position < (int)linear_.size() &&
			linear_[result.uses[0].position]->opcode == LOWIR_INS_COPYOBJ;
		for(int q = current_position_ + 1;
		    foldable_copy && q < result.uses[0].position; q++)
			if(linear_[q]->opcode != LOWIR_INS_ADDR &&
			   linear_[q]->opcode != LOWIR_INS_CONST)
				foldable_copy = false;
		if(foldable_copy) {
			ValueLocation location;
			location.kind = ValueLocation::VL_NONE;
			location.also_in_rax = true;
			locations_[ins.result] = location;
			rax_alias_ = ins.result;
			return;
		}
		long long offset = alloc_frame_home(
			ins.result, return_type, mir_model::FrameBinding::FB_TEMP);
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_R11));
		lea.operands.push_back(frame_operand(offset));
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = ContainerSpelling(return_type.obj_bytes);
		store.operands.push_back(MakeDeref(XR_R11, 0));
		store.operands.push_back(MakeReg(XR_RAX));
		ValueLocation location;
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = offset;
		locations_[ins.result] = location;
		return;
	}
	invalidate_rax();
	if(result_plan == RP_COLLAPSE) {
		ValueLocation location;
		location.kind = ValueLocation::VL_NONE;
		location.also_in_rax = true;
		locations_[ins.result] = location;
		rax_alias_ = ins.result;
	}
	else if(result_plan == RP_POOL) {
		mir_model::Instruction & copy = emit(mir_model::Instruction::MI_MOV);
		copy.result_copy_hint = true;
		copy.operands.push_back(MakeReg(result_reg));
		copy.operands.push_back(MakeReg(XR_RAX));
		locations_[ins.result].also_in_rax = true;
		rax_alias_ = ins.result;
	}
	else if(result_plan == RP_FRAME) {
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = SpellType(return_type);
		store.operands.push_back(frame_operand(result_offset));
		store.operands.push_back(MakeReg(XR_RAX));
		ValueLocation location;
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = result_offset;
		locations_[ins.result] = location;
	}
}

void FunctionLowering::LowerCopyObj(const LowIRInstruction & ins)
{
	const LowIROperand & source = ins.operands[0];
	const LowIROperand & target = ins.operands[1];
	if(source.kind == LOWIR_OPERAND_TEMP && operand_in_rax(source) &&
	   values_[source.name].type.kind == LOWIR_TYPE_OBJ) {
		mir_model::Operand address = address_operand(target, XR_R11);
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = ContainerSpelling((long long)ins.span_bytes);
		store.operands.push_back(address);
		store.operands.push_back(MakeReg(XR_RAX));
		return;
	}
	// destination first into rdi, then source into rsi
	mir_model::Operand dst = address_operand(target, XR_RDI);
	if(dst.kind == mir_model::Operand::OP_DEREF && dst.reg != XR_RDI)
		emit_mov(MakeReg(XR_RDI), MakeReg(dst.reg));
	else if(dst.kind == mir_model::Operand::OP_FRAME) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RDI));
		lea.operands.push_back(dst);
	}
	else if(dst.kind == mir_model::Operand::OP_GLOBAL) {
		emit_mov(MakeReg(XR_RDI), dst);
	}

	mir_model::Operand src;
	if(source.kind == LOWIR_OPERAND_TEMP &&
	   values_[source.name].type.kind == LOWIR_TYPE_OBJ &&
	   locations_[source.name].kind == ValueLocation::VL_FRAME) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RSI));
		lea.operands.push_back(
			frame_operand(locations_[source.name].frame_offset));
	}
	else {
		src = address_operand(source, XR_RSI);
		if(src.kind == mir_model::Operand::OP_DEREF && src.reg != XR_RSI)
			emit_mov(MakeReg(XR_RSI), MakeReg(src.reg));
		else if(src.kind == mir_model::Operand::OP_FRAME) {
			mir_model::Instruction & lea =
				emit(mir_model::Instruction::MI_LEA);
			lea.operands.push_back(MakeReg(XR_RSI));
			lea.operands.push_back(src);
		}
		else if(src.kind == mir_model::Operand::OP_GLOBAL) {
			emit_mov(MakeReg(XR_RSI), src);
		}
	}

	mir_model::Instruction & copy =
		emit(mir_model::Instruction::MI_COPY_BYTES);
	copy.byte_count = ins.span_bytes;
	copy.byte_alignment = ins.span_align > 0 ? ins.span_align : 1;
	copy.operands.push_back(MakeReg(XR_RDI));
	copy.operands.push_back(MakeReg(XR_RSI));
}

void FunctionLowering::LowerZeroInit(const LowIRInstruction & ins)
{
	const LowIROperand & target = ins.operands[0];
	mir_model::Operand dst = address_operand(target, XR_RDI);
	if(dst.kind == mir_model::Operand::OP_DEREF && dst.reg != XR_RDI)
		emit_mov(MakeReg(XR_RDI), MakeReg(dst.reg));
	else if(dst.kind == mir_model::Operand::OP_FRAME) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RDI));
		lea.operands.push_back(dst);
	}
	else if(dst.kind == mir_model::Operand::OP_GLOBAL) {
		emit_mov(MakeReg(XR_RDI), dst);
	}
	mir_model::Instruction & zero =
		emit(mir_model::Instruction::MI_ZERO_BYTES);
	zero.byte_count = ins.span_bytes;
	zero.byte_alignment = ins.span_align > 0 ? ins.span_align : 1;
	zero.operands.push_back(MakeReg(XR_RDI));
}

void FunctionLowering::LowerAtomicLoad(const LowIRInstruction & ins)
{
	LowIRInstruction plain = ins;
	plain.opcode = LOWIR_INS_LOAD;
	plain.operands.resize(1);
	LowerLoad(plain);
}

void FunctionLowering::LowerAtomicStore(const LowIRInstruction & ins)
{
	if(ins.atomic_order < 5) {
		LowIRInstruction plain = ins;
		plain.opcode = LOWIR_INS_STORE;
		plain.operands.resize(2);
		LowerStore(plain);
		return;
	}
	const LowIROperand & source = ins.operands[0];
	invalidate_rax();
	if(source.kind == LOWIR_OPERAND_LITERAL)
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(source)));
	else
		emit_mov(MakeReg(XR_RAX), gpr_read(source));
	mir_model::Operand address = address_operand(ins.operands[1], XR_RCX);
	mir_model::Instruction & xchg = emit(mir_model::Instruction::MI_XCHG);
	xchg.type = SpellType(ins.type);
	xchg.operands.push_back(address);
	xchg.operands.push_back(MakeReg(XR_RAX));
}

void FunctionLowering::LowerAtomicExchange(const LowIRInstruction & ins)
{
	const LowIROperand & source = ins.operands[1];
	invalidate_rax();
	if(source.kind == LOWIR_OPERAND_LITERAL)
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(source)));
	else
		emit_mov(MakeReg(XR_RAX), gpr_read(source));
	mir_model::Operand address = address_operand(ins.operands[0], XR_RCX);
	mir_model::Instruction & xchg = emit(mir_model::Instruction::MI_XCHG);
	xchg.type = SpellType(ins.type);
	xchg.operands.push_back(address);
	xchg.operands.push_back(MakeReg(XR_RAX));
	assign_result_from_rax(ins.result, ins.type, true);
}

void FunctionLowering::LowerAtomicAddFetch(const LowIRInstruction & ins)
{
	const LowIROperand & pointer = ins.operands[0];
	const LowIROperand & delta = ins.operands[1];
	if(pointer.kind == LOWIR_OPERAND_TEMP &&
	   locations_[pointer.name].kind == ValueLocation::VL_SLOT_ADDR) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RCX));
		lea.operands.push_back(frame_operand(
			slots_[locations_[pointer.name].slot_name].frame_offset));
	}
	else if(pointer.kind == LOWIR_OPERAND_GLOBAL) {
		emit_mov(MakeReg(XR_RCX), MakeSymbol(pointer.name, true));
	}
	else {
		emit_mov(MakeReg(XR_RCX), gpr_read(pointer));
	}
	if(delta.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(XR_RDX), MakeImm(ParseIntLiteral(delta)));
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(delta)));
	}
	else {
		emit_mov(MakeReg(XR_RDX), gpr_read(delta));
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), gpr_read(delta));
	}
	mir_model::Instruction & xadd =
		emit(mir_model::Instruction::MI_LOCK_XADD);
	xadd.type = SpellType(ins.type);
	xadd.operands.push_back(MakeDeref(XR_RCX, 0));
	xadd.operands.push_back(MakeReg(XR_RAX));
	mir_model::Instruction & add = emit(mir_model::Instruction::MI_ADD);
	add.operands.push_back(MakeReg(XR_RAX));
	add.operands.push_back(MakeReg(XR_RDX));
	assign_result_from_rax(ins.result, ins.type, true);
}

void FunctionLowering::LowerAtomicCompareExchange(
	const LowIRInstruction & ins)
{
	const LowIROperand & pointer = ins.operands[0];
	const LowIROperand & expected = ins.operands[1];
	const LowIROperand & desired = ins.operands[2];
	if(pointer.kind == LOWIR_OPERAND_GLOBAL)
		emit_mov(MakeReg(XR_RCX), MakeSymbol(pointer.name, true));
	else
		emit_mov(MakeReg(XR_RCX), gpr_read(pointer));
	mir_model::Operand expected_address =
		address_operand(expected, XR_RDX);
	if(expected_address.kind != mir_model::Operand::OP_DEREF ||
	   expected_address.reg != XR_RDX) {
		if(expected_address.kind == mir_model::Operand::OP_FRAME) {
			mir_model::Instruction & lea =
				emit(mir_model::Instruction::MI_LEA);
			lea.operands.push_back(MakeReg(XR_RDX));
			lea.operands.push_back(expected_address);
		}
		else if(expected_address.kind == mir_model::Operand::OP_DEREF) {
			emit_mov(MakeReg(XR_RDX), MakeReg(expected_address.reg));
		}
		else {
			emit_mov(MakeReg(XR_RDX), expected_address);
		}
	}
	invalidate_rax();
	mir_model::Instruction & load = emit(mir_model::Instruction::MI_LOAD);
	load.type = SpellType(ins.type);
	load.operands.push_back(MakeReg(XR_RAX));
	load.operands.push_back(MakeDeref(XR_RDX, 0));
	if(desired.kind == LOWIR_OPERAND_LITERAL)
		emit_mov(MakeReg(XR_RSI), MakeImm(ParseIntLiteral(desired)));
	else
		emit_mov(MakeReg(XR_RSI), gpr_read(desired));
	mir_model::Instruction & cmpxchg =
		emit(mir_model::Instruction::MI_LOCK_CMPXCHG);
	cmpxchg.type = SpellType(ins.type);
	cmpxchg.operands.push_back(MakeDeref(XR_RCX, 0));
	cmpxchg.operands.push_back(MakeReg(XR_RSI));
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = SpellType(ins.type);
	store.operands.push_back(MakeDeref(XR_RDX, 0));
	store.operands.push_back(MakeReg(XR_RAX));
	mir_model::Instruction & set = emit(mir_model::Instruction::MI_SETCC);
	set.condition = XC_E;
	set.operands.push_back(MakeReg(XR_RAX));
	mir_model::Instruction & widen = emit(mir_model::Instruction::MI_MOVZX);
	widen.operands.push_back(MakeReg(XR_RAX));
	widen.operands.push_back(MakeReg(XR_RAX));
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_I64;
	assign_result_from_rax(ins.result, result_type, false);
}

void FunctionLowering::LowerJump(const LowIRInstruction & ins)
{
	mir_model::Instruction & jmp = emit(mir_model::Instruction::MI_JMP);
	jmp.operands.push_back(MakeLabel(ins.block_targets[0]));
}

void FunctionLowering::emit_fused_compare(const LowIRInstruction & cmp,
                                          const std::string & true_label,
                                          const std::string & false_label)
{
	if(cmp.type.is_float()) {
		mir_model::Operand a = float_read(cmp.operands[0], cmp.type);
		mir_model::Operand b = float_read(cmp.operands[1], cmp.type);
		mir_model::Instruction & flags =
			emit(mir_model::Instruction::MI_FCMP);
		flags.type = SpellType(cmp.type);
		flags.operands.push_back(a);
		flags.operands.push_back(b);
		// Flags describe ucomis(b, a); parity means unordered.
		std::string pred = cmp.operation;
		std::string parity_target = pred == "ne" ? true_label : false_label;
		X86Condition condition;
		if(pred == "eq") condition = XC_E;
		else if(pred == "ne") condition = XC_NE;
		else if(pred == "gt" || pred == "ugt") condition = XC_B;
		else if(pred == "ge" || pred == "uge") condition = XC_BE;
		else if(pred == "lt" || pred == "ult") condition = XC_A;
		else if(pred == "le" || pred == "ule") condition = XC_AE;
		else
			throw std::runtime_error("unknown floating comparison: " + pred);
		mir_model::Instruction & jp = emit(mir_model::Instruction::MI_JCC);
		jp.condition = XC_P;
		jp.operands.push_back(MakeLabel(parity_target));
		mir_model::Instruction & jcc = emit(mir_model::Instruction::MI_JCC);
		jcc.condition = condition;
		jcc.operands.push_back(MakeLabel(true_label));
		mir_model::Instruction & jmp = emit(mir_model::Instruction::MI_JMP);
		jmp.operands.push_back(MakeLabel(false_label));
		return;
	}

	const LowIROperand & lhs = cmp.operands[0];
	const LowIROperand & rhs = cmp.operands[1];
	bool wide = cmp.type.kind == LOWIR_TYPE_I32 ||
	            cmp.type.kind == LOWIR_TYPE_U32 ||
	            cmp.type.kind == LOWIR_TYPE_I64 ||
	            cmp.type.kind == LOWIR_TYPE_PTR;
	bool rhs_literal = rhs.kind == LOWIR_OPERAND_LITERAL;
	long long cmp_width = FrameSizeOf(cmp.type);

	if(rhs.kind == LOWIR_OPERAND_TEMP &&
	   locations_[rhs.name].kind == ValueLocation::VL_PENDING_COPY)
		resolve_location(rhs.name);
	mir_model::Operand left;
	bool left_is_memory = false;
	if(operand_is_pending(lhs)) {
		const LowIRInstruction * load = take_pending(lhs);
		if(rhs_literal && wide) {
			left = address_operand(load->operands[0], XR_RCX);
			left_is_memory = true;
		}
		else {
			mir_model::Operand address =
				address_operand(load->operands[0], XR_RCX);
			invalidate_rax();
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_LOAD);
			fill.type = SpellType(load->type);
			fill.operands.push_back(MakeReg(XR_RAX));
			fill.operands.push_back(address);
			left = MakeReg(XR_RAX);
		}
		release_after_use(*load);
	}
	else if(lhs.kind == LOWIR_OPERAND_LITERAL) {
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(lhs)));
		left = MakeReg(XR_RAX);
	}
	else if(lhs.kind == LOWIR_OPERAND_SLOT) {
		if(rhs_literal &&
		   FrameSizeOf(slots_[lhs.name].type) == cmp_width) {
			left = frame_operand(slots_[lhs.name].frame_offset);
			left_is_memory = true;
		}
		else if(FrameSizeOf(slots_[lhs.name].type) != cmp_width) {
			left = MakeReg(reload_to_pool(lhs, slots_[lhs.name].type));
		}
		else {
			invalidate_rax();
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_LOAD);
			fill.type = SpellType(cmp.type);
			fill.operands.push_back(MakeReg(XR_RAX));
			fill.operands.push_back(
				frame_operand(slots_[lhs.name].frame_offset));
			left = MakeReg(XR_RAX);
		}
	}
	else if(operand_in_rax(lhs) &&
	        locations_[lhs.name].kind != ValueLocation::VL_GPR) {
		left = MakeReg(XR_RAX);
	}
	else if(lhs.kind == LOWIR_OPERAND_TEMP &&
	        locations_[lhs.name].kind == ValueLocation::VL_SLOT_ADDR) {
		invalidate_rax();
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RAX));
		lea.operands.push_back(frame_operand(
			slots_[locations_[lhs.name].slot_name].frame_offset));
		left = MakeReg(XR_RAX);
	}
	else {
		const ValueLocation & location = resolve_location(lhs.name);
		if(location.kind == ValueLocation::VL_FRAME) {
			if(rhs_literal && wide) {
				left = frame_operand(location.frame_offset);
				left_is_memory = true;
			}
			else {
				invalidate_rax();
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_LOAD);
				fill.type = SpellType(values_[lhs.name].type);
				fill.operands.push_back(MakeReg(XR_RAX));
				fill.operands.push_back(
					frame_operand(location.frame_offset));
				left = MakeReg(XR_RAX);
			}
		}
		else {
			left = gpr_read(lhs);
		}
	}

	mir_model::Operand right;
	if(rhs_literal) {
		long long value = ParseIntLiteral(rhs);
		bool inline_ok = wide && FitsImm32(value);
		if(inline_ok) {
			right = MakeImm(value);
		}
		else {
			if(left.kind == mir_model::Operand::OP_REG &&
			   left.reg != XR_RAX) {
				invalidate_rax();
				emit_mov(MakeReg(XR_RAX), left);
				left = MakeReg(XR_RAX);
			}
			emit_mov(MakeReg(XR_RDX), MakeImm(value));
			right = MakeReg(XR_RDX);
		}
	}
	else if(operand_is_pending(rhs) && !left_is_memory) {
		const LowIRInstruction * load = take_pending(rhs);
		right = address_operand(load->operands[0], XR_RDX);
		release_after_use(*load);
	}
	else if(rhs.kind == LOWIR_OPERAND_SLOT && !left_is_memory &&
	        FrameSizeOf(slots_[rhs.name].type) == cmp_width) {
		right = frame_operand(slots_[rhs.name].frame_offset);
	}
	else if(rhs.kind == LOWIR_OPERAND_SLOT) {
		right = MakeReg(reload_to_pool(rhs, slots_[rhs.name].type));
	}
	else if(rhs.kind == LOWIR_OPERAND_TEMP &&
	        resolve_location(rhs.name).kind == ValueLocation::VL_FRAME) {
		// spilled temps reload into a pool register at their own width
		right = MakeReg(reload_to_pool(rhs, values_[rhs.name].type));
	}
	else if(rhs.kind == LOWIR_OPERAND_TEMP &&
	        locations_[rhs.name].kind == ValueLocation::VL_SLOT_ADDR) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RDX));
		lea.operands.push_back(frame_operand(
			slots_[locations_[rhs.name].slot_name].frame_offset));
		right = MakeReg(XR_RDX);
	}
	else {
		right = gpr_read(rhs);
	}

	mir_model::Instruction & flags = emit(mir_model::Instruction::MI_CMP);
	flags.type = SpellType(cmp.type);
	flags.operands.push_back(left);
	flags.operands.push_back(right);
	mir_model::Instruction & jcc = emit(mir_model::Instruction::MI_JCC);
	jcc.condition = integer_condition(cmp.operation,
	                                  type_is_signed(cmp.type));
	jcc.operands.push_back(MakeLabel(true_label));
	mir_model::Instruction & jmp = emit(mir_model::Instruction::MI_JMP);
	jmp.operands.push_back(MakeLabel(false_label));
}

void FunctionLowering::LowerBranch(const LowIRInstruction & ins)
{
	std::string true_label = ins.block_targets[0];
	std::string false_label = ins.block_targets[1];
	bool invert = false;
	const LowIRInstruction * fused =
		fused_compare_for_branch(ins, current_position_, invert);
	if(fused) {
		if(invert)
			std::swap(true_label, false_label);
		emit_fused_compare(*fused, true_label, false_label);
		release_after_use(*fused);
		return;
	}

	// branch on a boolean value; unwrap adjacent single-use logical nots
	// by inverting the branch condition
	LowIROperand condition = ins.operands[0];
	bool inverted = false;
	int probe = current_position_ - 1;
	while(condition.kind == LOWIR_OPERAND_TEMP && probe >= 0) {
		const LowIRInstruction & prior = *linear_[probe];
		if(prior.opcode != LOWIR_INS_UNARY || prior.operation != "not" ||
		   prior.result != condition.name ||
		   values_[condition.name].uses.size() != 1)
			break;
		inverted = !inverted;
		condition = prior.operands[0];
		probe--;
	}

	LowIRType type;
	type.kind = LOWIR_TYPE_I64;
	if(condition.kind == LOWIR_OPERAND_TEMP)
		type = values_[condition.name].type;
	if(condition.kind == LOWIR_OPERAND_LITERAL) {
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(condition)));
	}
	else if(!operand_in_rax(condition)) {
		const ValueInfo & info = values_[condition.name];
		const ValueLocation & location = resolve_location(condition.name);
		invalidate_rax();
		if(info.is_param && !arg_homes_clobbered_ &&
		   location.prefer_home && current_block_ == 0 &&
		   location.kind == ValueLocation::VL_GPR) {
			// the incoming argument register still holds the value
			X64Register home = location.reg;
			for(size_t pb = 0; pb < out_.params.size(); pb++)
				if(out_.params[pb].name == condition.name &&
				   out_.params[pb].location ==
				       mir_model::ParamBinding::PL_REG)
					home = out_.params[pb].reg;
			emit_mov(MakeReg(XR_RAX), MakeReg(home));
		}
		else if(location.kind == ValueLocation::VL_FRAME) {
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_LOAD);
			fill.type = SpellType(type);
			fill.operands.push_back(MakeReg(XR_RAX));
			fill.operands.push_back(frame_operand(location.frame_offset));
		}
		else {
			emit_mov(MakeReg(XR_RAX), MakeReg(location.reg));
		}
	}
	mir_model::Instruction & flags = emit(mir_model::Instruction::MI_CMP);
	flags.type = SpellType(type);
	flags.operands.push_back(MakeReg(XR_RAX));
	flags.operands.push_back(MakeImm(0));
	mir_model::Instruction & jcc = emit(mir_model::Instruction::MI_JCC);
	jcc.condition = inverted ? XC_E : XC_NE;
	jcc.operands.push_back(MakeLabel(true_label));
	mir_model::Instruction & jmp = emit(mir_model::Instruction::MI_JMP);
	jmp.operands.push_back(MakeLabel(false_label));
}

void FunctionLowering::LowerSwitch(const LowIRInstruction & ins)
{
	const LowIROperand & selector = ins.operands[0];
	LowIRType type;
	type.kind = LOWIR_TYPE_I64;
	if(selector.kind == LOWIR_OPERAND_LITERAL) {
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(selector)));
	}
	else if(!operand_in_rax(selector)) {
		const ValueInfo & info = values_[selector.name];
		const ValueLocation & location = resolve_location(selector.name);
		invalidate_rax();
		if(info.is_param && !arg_homes_clobbered_ &&
		   location.prefer_home && current_block_ == 0 &&
		   location.kind == ValueLocation::VL_GPR) {
			// the incoming argument register still holds the value
			X64Register home = location.reg;
			for(size_t pb = 0; pb < out_.params.size(); pb++)
				if(out_.params[pb].name == selector.name &&
				   out_.params[pb].location ==
				       mir_model::ParamBinding::PL_REG)
					home = out_.params[pb].reg;
			emit_mov(MakeReg(XR_RAX), MakeReg(home));
		}
		else if(location.kind == ValueLocation::VL_FRAME) {
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_LOAD);
			fill.type = SpellType(type);
			fill.operands.push_back(MakeReg(XR_RAX));
			fill.operands.push_back(frame_operand(location.frame_offset));
		}
		else {
			emit_mov(MakeReg(XR_RAX), MakeReg(location.reg));
		}
	}
	for(size_t c = 0; c < ins.switch_values.size(); c++) {
		const LowIROperand & value = ins.switch_values[c];
		if(value.kind == LOWIR_OPERAND_LITERAL) {
			emit_mov(MakeReg(XR_RCX), MakeImm(ParseIntLiteral(value)));
		}
		else {
			const ValueLocation & location = resolve_location(value.name);
			if(location.kind == ValueLocation::VL_FRAME) {
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_LOAD);
				fill.type = SpellType(type);
				fill.operands.push_back(MakeReg(XR_RCX));
				fill.operands.push_back(
					frame_operand(location.frame_offset));
			}
			else {
				emit_mov(MakeReg(XR_RCX), MakeReg(location.reg));
			}
		}
		mir_model::Instruction & flags =
			emit(mir_model::Instruction::MI_CMP);
		flags.type = SpellType(type);
		flags.operands.push_back(MakeReg(XR_RAX));
		flags.operands.push_back(MakeReg(XR_RCX));
		mir_model::Instruction & jcc = emit(mir_model::Instruction::MI_JCC);
		jcc.condition = XC_E;
		jcc.operands.push_back(MakeLabel(ins.block_targets[1 + c]));
	}
	mir_model::Instruction & jmp = emit(mir_model::Instruction::MI_JMP);
	jmp.operands.push_back(MakeLabel(ins.block_targets[0]));
}

void FunctionLowering::LowerReturn(const LowIRInstruction & ins)
{
	if(ins.operands.empty() || ins.type.is_void()) {
		emit(mir_model::Instruction::MI_RET);
		return;
	}
	const LowIROperand & source = ins.operands[0];
	if(ins.type.is_f80()) {
		mir_model::Operand value = float_read(source, ins.type);
		if(value.kind == mir_model::Operand::OP_FLOAT_IMM) {
			// materialize a literal into a transient frame home first
			long long offset = alloc_frame_home(
				"__ret80", ins.type, mir_model::FrameBinding::FB_TEMP);
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = "f80";
			mov.operands.push_back(frame_operand(offset));
			mov.operands.push_back(value);
			value = frame_operand(offset);
		}
		mir_model::Instruction & ret = emit(mir_model::Instruction::MI_FRET);
		ret.type = "f80";
		ret.operands.push_back(value);
		return;
	}
	if(ins.type.kind == LOWIR_TYPE_F32 || ins.type.kind == LOWIR_TYPE_F64) {
		LowIRType value_type = ins.type;
		if(source.kind == LOWIR_OPERAND_TEMP &&
		   values_.count(source.name) &&
		   values_[source.name].type.is_float())
			value_type = values_[source.name].type;
		mir_model::Operand value = float_read(source, value_type);
		if(value_type.kind != ins.type.kind) {
			bool widen = FrameSizeOf(ins.type) > FrameSizeOf(value_type);
			mir_model::Instruction & convert =
				emit(widen ? mir_model::Instruction::MI_FPEXT
				           : mir_model::Instruction::MI_FPTRUNC);
			convert.type = SpellType(ins.type);
			convert.source_type = SpellType(value_type);
			convert.operands.push_back(MakeXmm(XMM_0));
			convert.operands.push_back(value);
			emit(mir_model::Instruction::MI_RET);
			return;
		}
		mir_model::Instruction & mov =
			emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(ins.type);
		mov.operands.push_back(MakeXmm(XMM_0));
		mov.operands.push_back(value);
		emit(mir_model::Instruction::MI_RET);
		return;
	}
	if(ins.type.kind == LOWIR_TYPE_OBJ) {
		long long offset = 0;
		if(source.kind == LOWIR_OPERAND_SLOT)
			offset = slots_[source.name].frame_offset;
		else if(source.kind == LOWIR_OPERAND_TEMP &&
		        locations_[source.name].kind == ValueLocation::VL_FRAME)
			offset = locations_[source.name].frame_offset;
		else
			throw std::runtime_error("object return without frame storage");
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_R11));
		lea.operands.push_back(frame_operand(offset));
		invalidate_rax();
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = ContainerSpelling(ins.type.obj_bytes);
		load.operands.push_back(MakeReg(XR_RAX));
		load.operands.push_back(MakeDeref(XR_R11, 0));
		emit(mir_model::Instruction::MI_RET);
		return;
	}
	if(source.kind == LOWIR_OPERAND_LITERAL) {
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(source)));
	}
	else if(source.kind == LOWIR_OPERAND_GLOBAL) {
		invalidate_rax();
		emit_mov(MakeReg(XR_RAX),
		         MakeSymbol(source.name,
		                    !facts_.info->is_function(source.name)));
	}
	else if(operand_is_pending(source)) {
		const LowIRInstruction * load = take_pending(source);
		mir_model::Operand address =
			address_operand(load->operands[0], XR_RCX);
		release_after_use(*load);
		invalidate_rax();
		mir_model::Instruction & fill =
			emit(mir_model::Instruction::MI_LOAD);
		fill.type = SpellType(load->type);
		fill.operands.push_back(MakeReg(XR_RAX));
		fill.operands.push_back(address);
	}
	else if(!operand_in_rax(source)) {
		const ValueLocation & location = resolve_location(source.name);
		invalidate_rax();
		if(location.kind == ValueLocation::VL_FRAME) {
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_LOAD);
			fill.type = SpellType(ins.type);
			fill.operands.push_back(MakeReg(XR_RAX));
			fill.operands.push_back(frame_operand(location.frame_offset));
		}
		else {
			emit_mov(MakeReg(XR_RAX), MakeReg(location.reg));
		}
	}
	mir_model::Instruction & ret = emit(mir_model::Instruction::MI_RET);
	ret.operands.push_back(MakeReg(XR_RAX));
}

}  // namespace lowir_to_mir
