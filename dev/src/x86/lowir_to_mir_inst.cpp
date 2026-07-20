#include "x86/lowir_to_mir.h"

#include <stdexcept>

// Data-instruction templates for the LowIR -> MIR lowering: constants,
// copies, memory access, integer/float arithmetic, comparisons-as-values,
// and conversions. Shapes follow the pa28 strict fixtures exactly.

namespace lowir_to_mir {

namespace {

std::string IntWidthSpelling(const LowIRType & type)
{
	switch(type.kind) {
		case LOWIR_TYPE_I1: return "i1";
		case LOWIR_TYPE_I8:
		case LOWIR_TYPE_U8: return "i8";
		case LOWIR_TYPE_I16:
		case LOWIR_TYPE_U16: return "i16";
		case LOWIR_TYPE_I32:
		case LOWIR_TYPE_U32: return "i32";
		default: return "i64";
	}
}

}  // namespace

bool FunctionLowering::type_is_signed(const LowIRType & type) const
{
	switch(type.kind) {
		case LOWIR_TYPE_I8:
		case LOWIR_TYPE_I16:
		case LOWIR_TYPE_I32:
		case LOWIR_TYPE_I64: return true;
		default: return false;
	}
}

X86Condition FunctionLowering::integer_condition(const std::string & pred,
                                                 bool sign) const
{
	if(pred == "eq") return XC_E;
	if(pred == "ne") return XC_NE;
	if(pred == "lt") return sign ? XC_L : XC_B;
	if(pred == "le") return sign ? XC_LE : XC_BE;
	if(pred == "gt") return sign ? XC_G : XC_A;
	if(pred == "ge") return sign ? XC_GE : XC_AE;
	if(pred == "ult") return XC_B;
	if(pred == "ule") return XC_BE;
	if(pred == "ugt") return XC_A;
	if(pred == "uge") return XC_AE;
	throw std::runtime_error("unknown comparison predicate: " + pred);
}

bool FunctionLowering::storage_is_tls(const LowIROperand & operand) const
{
	return StorageIsTls(operand, facts_);
}

void FunctionLowering::emit_tls_addr(const std::string & global_name)
{
	// The address materialization is a call boundary (the host model
	// calls the per-TU wrapper): a value mirrored in rax is stale on
	// the far side and must re-read its frame home.
	invalidate_rax();
	std::map<std::string, std::string>::const_iterator it =
		facts_.tls_wrapper_of_global.find(global_name);
	mir_model::Instruction & tls = emit(mir_model::Instruction::MI_TLS_ADDR);
	tls.operands.push_back(MakeReg(XR_R11));
	tls.operands.push_back(MakeSymbol(it->second, false));
}

// -- pending single-use loads -----------------------------------------

// Finds the compare that lowers as part of this branch: the block-local
// single-use compare defining the branch condition (any distance — the
// compare sinks to the branch), or the compare under one adjacent
// logical not.
const LowIRInstruction * FunctionLowering::fused_compare_for_branch(
	const LowIRInstruction & branch, int position, bool & invert) const
{
	invert = false;
	if(branch.operands[0].kind != LOWIR_OPERAND_TEMP || position < 1)
		return 0;
	int block_begin = enclosing_block_begin(position);
	for(int p = position - 1; p >= block_begin; p--) {
		const LowIRInstruction & prior = *linear_[p];
		if(prior.result != branch.operands[0].name)
			continue;
		std::map<std::string, ValueInfo>::const_iterator it =
			values_.find(prior.result);
		if(it == values_.end() || it->second.uses.size() != 1)
			return 0;
		if(prior.opcode == LOWIR_INS_CMP)
			// PA29: 128-bit compares lower through the pair path and
			// never fuse into the branch.
			return prior.type.kind == LOWIR_TYPE_I128 ? 0 : &prior;
		if(prior.opcode == LOWIR_INS_UNARY && prior.operation == "not" &&
		   prior.operands[0].kind == LOWIR_OPERAND_TEMP && p >= 1 &&
		   p == position - 1) {
			const LowIRInstruction & inner = *linear_[p - 1];
			if(inner.opcode == LOWIR_INS_CMP &&
			   inner.type.kind != LOWIR_TYPE_I128 &&
			   inner.result == prior.operands[0].name) {
				std::map<std::string, ValueInfo>::const_iterator inner_it =
					values_.find(inner.result);
				if(inner_it != values_.end() &&
				   inner_it->second.uses.size() == 1) {
					invert = true;
					return &inner;
				}
			}
		}
		return 0;
	}
	return 0;
}

// A load may sink into the next instruction when its single consumer can
// read memory (or the rax staging path) directly.
bool FunctionLowering::try_defer_load(const LowIRInstruction & ins,
                                      int position)
{
	std::map<std::string, ValueInfo>::const_iterator it =
		values_.find(ins.result);
	if(it == values_.end() || it->second.uses.size() != 1 ||
	   it->second.needs_frame || it->second.cross_block)
		return false;
	ValueUse use = it->second.uses[0];
	// retimed (branch-sunk) compares record their use at the branch;
	// stable-storage loads ride along to the branch with the compare
	if(use.position != position + 1) {
		if(use.position < (int)linear_.size() &&
		   linear_[use.position]->opcode == LOWIR_INS_BRANCH) {
			bool invert = false;
			const LowIRInstruction * sunk = fused_compare_for_branch(
				*linear_[use.position], use.position, invert);
			bool references = false;
			if(sunk)
				for(size_t o = 0; o < sunk->operands.size(); o++)
					if(sunk->operands[o].kind == LOWIR_OPERAND_TEMP &&
					   sunk->operands[o].name == ins.result)
						references = true;
			bool stable = ins.operands[0].kind == LOWIR_OPERAND_SLOT ||
			              ins.operands[0].kind == LOWIR_OPERAND_GLOBAL;
			if(references && stable &&
			   FrameSizeOf(ins.type) == FrameSizeOf(sunk->type) &&
			   !ins.type.is_float()) {
				pending_loads_[ins.result] = &ins;
				int slot = pool_scan(false, false);
				if(slot >= 0)
					pool_holder_[slot] = "*pending:" + ins.result;
				return true;
			}
		}
		if(position + 1 >= (int)linear_.size())
			return false;
		const LowIRInstruction & next = *linear_[position + 1];
		bool referenced = false;
		for(size_t o = 0; o < next.operands.size(); o++)
			if(next.operands[o].kind == LOWIR_OPERAND_TEMP &&
			   next.operands[o].name == ins.result)
				referenced = true;
		if(!referenced)
			return false;
		use.position = position + 1;
	}
	const LowIRInstruction & user = *linear_[use.position];
	bool foldable = false;
	if(use.kind == ValueUse::USE_STORE_VALUE &&
	   user.opcode == LOWIR_INS_STORE)
		foldable = true;
	else if(use.kind == ValueUse::USE_RETURN_VALUE &&
	        (ins.type.kind == LOWIR_TYPE_I64 ||
	         ins.type.kind == LOWIR_TYPE_PTR))
		foldable = true;
	else if(use.kind == ValueUse::USE_CALL_ARG &&
	        (ins.type.kind == LOWIR_TYPE_I64 ||
	         ins.type.kind == LOWIR_TYPE_PTR))
		foldable = true;
	else if(user.opcode == LOWIR_INS_CMP && !ins.type.is_float() &&
	        FrameSizeOf(ins.type) == FrameSizeOf(user.type)) {
		// the compare must sink into its block's branch; loads may ride
		// along past intervening instructions only from stable storage
		int cmp_block_end = enclosing_block_end(use.position);
		bool invert = false;
		if(cmp_block_end - 1 < (int)linear_.size() &&
		   linear_[cmp_block_end - 1]->opcode == LOWIR_INS_BRANCH &&
		   fused_compare_for_branch(*linear_[cmp_block_end - 1],
		                            cmp_block_end - 1, invert) == &user) {
			bool stable = ins.operands[0].kind == LOWIR_OPERAND_SLOT ||
			              ins.operands[0].kind == LOWIR_OPERAND_GLOBAL;
			if(stable)
				foldable = true;
		}
	}
	if(!foldable)
		return false;
	pending_loads_[ins.result] = &ins;
	// non-return consumers hold a pool slot for the value's would-be home
	if(use.kind != ValueUse::USE_RETURN_VALUE) {
		int index = pool_scan(false, false);
		if(index >= 0)
			pool_holder_[index] = "*pending:" + ins.result;
	}
	return true;
}

bool FunctionLowering::operand_is_pending(const LowIROperand & operand) const
{
	return operand.kind == LOWIR_OPERAND_TEMP &&
	       pending_loads_.count(operand.name) != 0;
}

const LowIRInstruction * FunctionLowering::take_pending(
	const LowIROperand & operand)
{
	const LowIRInstruction * load = pending_loads_[operand.name];
	pending_loads_.erase(operand.name);
	for(int i = 0; i < kPoolSize; i++)
		if(pool_holder_[i] == "*pending:" + operand.name)
			pool_holder_[i] = "";
	return load;
}

// -- shared result placement ------------------------------------------

void FunctionLowering::assign_result_from_rax(const std::string & dest,
                                              const LowIRType & type,
                                              bool normalize)
{
	invalidate_rax();
	ValueInfo & info = values_[dest];
	if(info.uses.empty())
		return;
	if(info.uses.size() == 1 && !info.needs_frame &&
	   info.uses[0].position == current_position_ + 1 &&
	   info.uses[0].kind == ValueUse::USE_RETURN_VALUE && !normalize) {
		ValueLocation location;
		location.kind = ValueLocation::VL_NONE;
		location.also_in_rax = true;
		locations_[dest] = location;
		rax_alias_ = dest;
		return;
	}
	if(info.needs_frame) {
		LowIRType home_type = type;
		if(FrameSizeOf(home_type) < 8 && home_type.kind != LOWIR_TYPE_OBJ)
			home_type.kind = LOWIR_TYPE_I64;
		long long offset = alloc_frame_home(dest, home_type,
		                                    mir_model::FrameBinding::FB_TEMP);
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = SpellType(type);
		store.operands.push_back(frame_operand(offset));
		store.operands.push_back(MakeReg(XR_RAX));
		ValueLocation location;
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = offset;
		locations_[dest] = location;
		return;
	}
	X64Register reg = XR_RAX;
	if(alloc_pool_gpr(dest, reg)) {
		emit_mov(MakeReg(reg), MakeReg(XR_RAX));
		if(normalize)
			emit_narrow_normalize(type, reg);
		locations_[dest].also_in_rax = !normalize;
		if(!normalize)
			rax_alias_ = dest;
		return;
	}
	LowIRType tail_type = type;
	if(FrameSizeOf(tail_type) < 8 && tail_type.kind != LOWIR_TYPE_OBJ)
		tail_type.kind = LOWIR_TYPE_I64;
	long long offset = alloc_frame_home(dest, tail_type,
	                                    mir_model::FrameBinding::FB_TEMP);
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = SpellType(tail_type);
	store.operands.push_back(frame_operand(offset));
	store.operands.push_back(MakeReg(XR_RAX));
	ValueLocation location;
	location.kind = ValueLocation::VL_FRAME;
	location.frame_offset = offset;
	locations_[dest] = location;
}

mir_model::Operand FunctionLowering::stage_store_value(
	const LowIROperand & operand, const LowIRType & type)
{
	if(operand.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(operand)));
		return MakeReg(XR_RAX);
	}
	if(operand.kind == LOWIR_OPERAND_GLOBAL) {
		emit_global_address(XR_RAX, operand.name);
		return MakeReg(XR_RAX);
	}
	if(operand.kind == LOWIR_OPERAND_SLOT) {
		invalidate_rax();
		mir_model::Instruction & fill =
			emit(mir_model::Instruction::MI_LOAD);
		fill.type = SpellType(type);
		fill.operands.push_back(MakeReg(XR_RAX));
		fill.operands.push_back(
			frame_operand(slots_[operand.name].frame_offset));
		return MakeReg(XR_RAX);
	}
	if(operand_is_pending(operand)) {
		const LowIRInstruction * load = pending_loads_[operand.name];
		mir_model::Operand address =
			address_operand(load->operands[0], XR_RCX);
		take_pending(operand);
		release_after_use(*load);
		invalidate_rax();
		mir_model::Instruction & fill =
			emit(mir_model::Instruction::MI_LOAD);
		fill.type = SpellType(load->type);
		fill.operands.push_back(MakeReg(XR_RAX));
		fill.operands.push_back(address);
		return MakeReg(XR_RAX);
	}
	const ValueLocation & location = resolve_location(operand.name);
	if(location.kind == ValueLocation::VL_GPR ||
	   location.kind == ValueLocation::VL_ARG_REG)
		return MakeReg(location.reg);
	if(location.kind == ValueLocation::VL_FRAME) {
		invalidate_rax();
		mir_model::Instruction & fill =
			emit(mir_model::Instruction::MI_LOAD);
		fill.type = SpellType(type);
		fill.operands.push_back(MakeReg(XR_RAX));
		fill.operands.push_back(frame_operand(location.frame_offset));
		return MakeReg(XR_RAX);
	}
	if(location.kind == ValueLocation::VL_SLOT_ADDR) {
		invalidate_rax();
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(XR_RAX));
		lea.operands.push_back(
			frame_operand(slots_[location.slot_name].frame_offset));
		return MakeReg(XR_RAX);
	}
	throw std::logic_error("store value without a location");
}

mir_model::Operand FunctionLowering::float_read(const LowIROperand & operand,
                                                const LowIRType & type)
{
	if(operand.kind == LOWIR_OPERAND_LITERAL)
		return MakeFloatImm(operand, type);
	if(operand_is_pending(operand)) {
		const LowIRInstruction * load = pending_loads_[operand.name];
		mir_model::Operand address =
			address_operand(load->operands[0], XR_RCX);
		take_pending(operand);
		release_after_use(*load);
		return address;
	}
	if(operand.kind == LOWIR_OPERAND_TEMP) {
		const ValueLocation & location = locations_[operand.name];
		if(location.kind == ValueLocation::VL_XMM)
			return MakeXmm(location.xmm);
		if(location.kind == ValueLocation::VL_FRAME)
			return frame_operand(location.frame_offset);
	}
	throw std::runtime_error("unsupported floating operand");
}

long long FunctionLowering::f80_result_home(const std::string & dest)
{
	LowIRType f80;
	f80.kind = LOWIR_TYPE_F80;
	long long offset = alloc_frame_home(dest, f80,
	                                    mir_model::FrameBinding::FB_TEMP);
	ValueLocation location;
	location.kind = ValueLocation::VL_FRAME;
	location.frame_offset = offset;
	locations_[dest] = location;
	return offset;
}

// -- instruction templates --------------------------------------------

void FunctionLowering::LowerConst(const LowIRInstruction & ins)
{
	if(ins.type.is_float()) {
		if(ins.type.is_f80() || values_[ins.result].crosses_call) {
			long long offset = f80_result_home(ins.result);
			if(!ins.type.is_f80()) {
				locations_[ins.result].kind = ValueLocation::VL_FRAME;
			}
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = SpellType(ins.type);
			mov.operands.push_back(frame_operand(offset));
			mov.operands.push_back(MakeFloatImm(ins.operands[0], ins.type));
			return;
		}
		XmmRegister xmm = alloc_xmm(ins.result);
		mir_model::Instruction & mov = emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(ins.type);
		mov.operands.push_back(MakeXmm(xmm));
		mov.operands.push_back(MakeFloatImm(ins.operands[0], ins.type));
		return;
	}
	X64Register reg = alloc_gpr(ins.result);
	if(locations_[ins.result].kind == ValueLocation::VL_FRAME) {
		emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(ins.operands[0])));
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = SpellType(ins.type);
		store.operands.push_back(
			frame_operand(locations_[ins.result].frame_offset));
		store.operands.push_back(MakeReg(XR_RAX));
		return;
	}
	emit_mov(MakeReg(reg), MakeImm(ParseIntLiteral(ins.operands[0])));
	emit_narrow_normalize(ins.type, reg);
}

void FunctionLowering::LowerCopy(const LowIRInstruction & ins)
{
	if(ins.type.is_float()) {
		mir_model::Operand source = float_read(ins.operands[0], ins.type);
		if(ins.type.is_f80() || values_[ins.result].crosses_call) {
			long long offset = f80_result_home(ins.result);
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = SpellType(ins.type);
			mov.operands.push_back(frame_operand(offset));
			mov.operands.push_back(source);
			return;
		}
		XmmRegister xmm = alloc_xmm(ins.result);
		mir_model::Instruction & mov = emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(ins.type);
		mov.operands.push_back(MakeXmm(xmm));
		mov.operands.push_back(source);
		return;
	}
	X64Register reg = XR_RAX;
	emit_dest_copy(ins.result, ins.operands[0], ins.type, true, reg);
	commit_dest(ins.result);
}

void FunctionLowering::LowerAddr(const LowIRInstruction & ins)
{
	const LowIROperand & target = ins.operands[0];
	if(target.kind == LOWIR_OPERAND_SLOT) {
		ValueLocation location;
		location.kind = ValueLocation::VL_SLOT_ADDR;
		location.slot_name = target.name;
		locations_[ins.result] = location;
		return;
	}
	if(storage_is_tls(target)) {
		emit_tls_addr(target.name);
		X64Register reg = alloc_gpr(ins.result);
		if(locations_[ins.result].kind == ValueLocation::VL_FRAME) {
			invalidate_rax();
			emit_mov(MakeReg(XR_RAX), MakeReg(XR_R11));
			commit_frame_result(ins.result);
			return;
		}
		emit_mov(MakeReg(reg), MakeReg(XR_R11));
		return;
	}
	X64Register reg = alloc_gpr(ins.result);
	bool is_global = !facts_.info->is_function(target.name);
	if(locations_[ins.result].kind == ValueLocation::VL_FRAME) {
		invalidate_rax();
		if(is_global)
			emit_global_address(XR_RAX, target.name);
		else
			emit_mov(MakeReg(XR_RAX), MakeSymbol(target.name, false));
		commit_frame_result(ins.result);
		return;
	}
	if(is_global)
		emit_global_address(reg, target.name);
	else
		emit_mov(MakeReg(reg), MakeSymbol(target.name, false));
}

void FunctionLowering::LowerLoad(const LowIRInstruction & ins)
{
	const LowIROperand & source = ins.operands[0];
	if(source.kind == LOWIR_OPERAND_SLOT && slots_[source.name].promoted) {
		if(ins.type.is_float()) {
			LowIRInstruction copy = ins;
			copy.opcode = LOWIR_INS_COPY;
			copy.operands.clear();
			copy.operands.push_back(promoted_slot_value_[source.name]);
			LowerCopy(copy);
			return;
		}
		X64Register reg = XR_RAX;
		emit_dest_copy(ins.result, promoted_slot_value_[source.name],
		               ins.type, false, reg);
		commit_dest(ins.result);
		return;
	}
	if(storage_is_tls(source)) {
		emit_tls_addr(source.name);
		X64Register reg = alloc_gpr(ins.result);
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = SpellType(ins.type);
		load.operands.push_back(MakeReg(reg));
		load.operands.push_back(MakeDeref(XR_R11, 0));
		emit_narrow_normalize(ins.type, reg);
		// a pool-exhausted result is frame-homed and rax only stages
		// it; the home must hold the value like every rax-staged load
		if(locations_[ins.result].kind == ValueLocation::VL_FRAME)
			commit_frame_result(ins.result);
		return;
	}
	if(try_defer_load(ins, current_position_))
		return;
	if(ins.type.is_float()) {
		mir_model::Operand address = address_operand(source, XR_RCX);
		if(ins.type.is_f80() || values_[ins.result].crosses_call) {
			long long offset = f80_result_home(ins.result);
			mir_model::Instruction & mov =
				emit(mir_model::Instruction::MI_FMOV);
			mov.type = SpellType(ins.type);
			mov.operands.push_back(frame_operand(offset));
			mov.operands.push_back(address);
			return;
		}
		XmmRegister xmm = alloc_xmm(ins.result);
		mir_model::Instruction & mov = emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(ins.type);
		mov.operands.push_back(MakeXmm(xmm));
		mov.operands.push_back(address);
		return;
	}
	mir_model::Operand address = address_operand(source, XR_RCX);
	X64Register reg = alloc_gpr(ins.result);
	if(locations_[ins.result].kind == ValueLocation::VL_FRAME) {
		invalidate_rax();
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = SpellType(ins.type);
		load.operands.push_back(MakeReg(XR_RAX));
		load.operands.push_back(address);
		emit_narrow_normalize(ins.type, XR_RAX);
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = "i64";
		store.operands.push_back(
			frame_operand(locations_[ins.result].frame_offset));
		store.operands.push_back(MakeReg(XR_RAX));
		return;
	}
	mir_model::Instruction & load = emit(mir_model::Instruction::MI_LOAD);
	load.type = SpellType(ins.type);
	load.operands.push_back(MakeReg(reg));
	load.operands.push_back(address);
	emit_narrow_normalize(ins.type, reg);
}

void FunctionLowering::LowerStore(const LowIRInstruction & ins)
{
	const LowIROperand & target = ins.operands[1];
	if(target.kind == LOWIR_OPERAND_SLOT && slots_[target.name].promoted)
		return;   // rewritten away by slot promotion
	if(storage_is_tls(target)) {
		int index = pool_scan(true, false);
		const LowIROperand & source = ins.operands[0];
		X64Register staged;
		if(index >= 0) {
			staged = kPool[index];
			if(source.kind == LOWIR_OPERAND_LITERAL)
				emit_mov(MakeReg(staged),
				         MakeImm(ParseIntLiteral(source)));
			else if(operand_is_pending(source)) {
				// a deferred single-use load folds into the staging
				// register (it must survive the wrapper call below)
				const LowIRInstruction * load =
					pending_loads_[source.name];
				mir_model::Operand address =
					address_operand(load->operands[0], XR_RCX);
				take_pending(source);
				release_after_use(*load);
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_LOAD);
				fill.type = SpellType(load->type);
				fill.operands.push_back(MakeReg(staged));
				fill.operands.push_back(address);
			}
			else
				emit_mov(MakeReg(staged), gpr_read(source));
		}
		else if(source.kind == LOWIR_OPERAND_LITERAL) {
			invalidate_rax();
			emit_mov(MakeReg(XR_RAX), MakeImm(ParseIntLiteral(source)));
			staged = XR_RAX;
		}
		else {
			mir_model::Operand value = stage_store_value(source, ins.type);
			staged = value.reg;
		}
		// The address materialization below is a call boundary: a
		// value staged outside the callee-saved pool rides an
		// anonymous spill across it.
		bool survives_call = staged == XR_RBX || staged == XR_R12 ||
			staged == XR_R13 || staged == XR_R14 || staged == XR_R15;
		long long spill = 0;
		if(!survives_call) {
			spill = alloc_anonymous_spill();
			mir_model::Instruction & save =
				emit(mir_model::Instruction::MI_STORE);
			save.type = "i64";
			save.operands.push_back(frame_operand(spill));
			save.operands.push_back(MakeReg(staged));
		}
		emit_tls_addr(target.name);
		if(!survives_call) {
			mir_model::Instruction & reload =
				emit(mir_model::Instruction::MI_LOAD);
			reload.type = "i64";
			reload.operands.push_back(MakeReg(XR_RAX));
			reload.operands.push_back(frame_operand(spill));
			staged = XR_RAX;
		}
		mir_model::Instruction & store =
			emit(mir_model::Instruction::MI_STORE);
		store.type = SpellType(ins.type);
		store.operands.push_back(MakeDeref(XR_R11, 0));
		store.operands.push_back(MakeReg(staged));
		return;
	}
	if(ins.type.is_float()) {
		LowIRType value_type = ins.type;
		if(ins.operands[0].kind == LOWIR_OPERAND_TEMP &&
		   values_.count(ins.operands[0].name) &&
		   values_[ins.operands[0].name].type.is_float())
			value_type = values_[ins.operands[0].name].type;
		mir_model::Operand value = float_read(ins.operands[0], value_type);
		mir_model::Operand address = address_operand(target, XR_RCX);
		if(value_type.kind != ins.type.kind &&
		   ins.operands[0].kind == LOWIR_OPERAND_TEMP) {
			bool widen = FrameSizeOf(ins.type) > FrameSizeOf(value_type);
			mir_model::Instruction & convert =
				emit(widen ? mir_model::Instruction::MI_FPEXT
				           : mir_model::Instruction::MI_FPTRUNC);
			convert.type = SpellType(ins.type);
			convert.source_type = SpellType(value_type);
			convert.operands.push_back(address);
			convert.operands.push_back(value);
			return;
		}
		mir_model::Instruction & mov = emit(mir_model::Instruction::MI_FMOV);
		mov.type = SpellType(ins.type);
		mov.operands.push_back(address);
		mov.operands.push_back(value);
		return;
	}
	mir_model::Operand value = stage_store_value(ins.operands[0], ins.type);
	mir_model::Operand address = address_operand(target, XR_RCX);
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = SpellType(ins.type);
	store.operands.push_back(address);
	store.operands.push_back(value);
}

void FunctionLowering::LowerIndex(const LowIRInstruction & ins)
{
	long long element = ins.type.element_bytes();
	X64Register reg = XR_RAX;
	LowIRType ptr;
	ptr.kind = LOWIR_TYPE_PTR;
	const LowIROperand & base = ins.operands[0];
	bool identity = ins.operands[1].kind == LOWIR_OPERAND_LITERAL &&
	                ParseIntLiteral(ins.operands[1]) * element == 0;
	bool base_callee_saved = false;
	if(base.kind == LOWIR_OPERAND_TEMP && value_pinned(base.name)) {
		const ValueLocation & bloc = locations_[base.name];
		if(bloc.kind == ValueLocation::VL_GPR &&
		   (bloc.reg == XR_RBX ||
		    (bloc.reg >= XR_R12 && bloc.reg <= XR_R15)))
			base_callee_saved = true;
	}
	index_dest_lowering_ = true;
	// The pinned-base fast path folds the element offset into one lea,
	// so it only applies to literal counts; runtime counts take the
	// general scale-in-rdx path below.
	if(base_callee_saved && !value_dies_here(base.name) &&
	   ins.operands[1].kind == LOWIR_OPERAND_LITERAL) {
		int index = pool_scan(true, false);
		if(index >= 0) {
			reg = kPool[index];
			pool_holder_[index] = ins.result;
			pool_clobbered_[index] = true;
			ValueLocation location;
			location.kind = ValueLocation::VL_GPR;
			location.reg = reg;
			locations_[ins.result] = location;
			emit_mov(MakeReg(reg),
			         MakeReg(locations_[base.name].reg));
			if(!identity) {
				mir_model::Instruction & lea =
					emit(mir_model::Instruction::MI_LEA);
				lea.operands.push_back(MakeReg(reg));
				lea.operands.push_back(MakeDeref(reg,
					ParseIntLiteral(ins.operands[1]) * element));
			}
			index_dest_lowering_ = false;
			return;
		}
	}
	emit_dest_copy(ins.result, base, ptr, false, reg);
	index_dest_lowering_ = false;
	if(identity && base.kind == LOWIR_OPERAND_TEMP) {
		ValueLocation & source = locations_[base.name];
		if(source.kind == ValueLocation::VL_ARG_REG &&
		   values_[base.name].last_use() <=
		       values_[ins.result].last_use()) {
			source.kind = ValueLocation::VL_GPR;
			source.reg = reg;
		}
	}
	const LowIROperand & count = ins.operands[1];
	if(count.kind == LOWIR_OPERAND_LITERAL) {
		long long offset = ParseIntLiteral(count) * element;
		if(offset != 0) {
			mir_model::Instruction & lea =
				emit(mir_model::Instruction::MI_LEA);
			lea.operands.push_back(MakeReg(reg));
			lea.operands.push_back(MakeDeref(reg, offset));
		}
		commit_dest(ins.result);
		return;
	}
	// Runtime element count: scale in rdx, then add.
	emit_mov(MakeReg(XR_RDX), gpr_read(count));
	if(element != 1) {
		mir_model::Instruction & mul =
			emit(mir_model::Instruction::MI_IMUL);
		mul.operands.push_back(MakeReg(XR_RDX));
		mul.operands.push_back(MakeImm(element));
	}
	mir_model::Instruction & add = emit(mir_model::Instruction::MI_ADD);
	add.operands.push_back(MakeReg(reg));
	add.operands.push_back(MakeReg(XR_RDX));
	commit_dest(ins.result);
}

void FunctionLowering::LowerUnary(const LowIRInstruction & ins)
{
	if(ins.type.is_float()) {
		if(ins.operation != "neg")
			throw std::runtime_error("unknown float unary operation: " +
			                         ins.operation);
		mir_model::Operand source = float_read(ins.operands[0], ins.type);
		if(ins.type.is_f80() || values_[ins.result].crosses_call) {
			long long offset = f80_result_home(ins.result);
			mir_model::Instruction & neg =
				emit(mir_model::Instruction::MI_FNEG);
			neg.type = SpellType(ins.type);
			neg.operands.push_back(frame_operand(offset));
			neg.operands.push_back(source);
			return;
		}
		XmmRegister xmm = alloc_xmm(ins.result);
		mir_model::Instruction & neg = emit(mir_model::Instruction::MI_FNEG);
		neg.type = SpellType(ins.type);
		neg.operands.push_back(MakeXmm(xmm));
		neg.operands.push_back(source);
		return;
	}
	X64Register reg = XR_RAX;
	if(ins.operation == "decay") {
		emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
		commit_dest(ins.result);
		return;
	}
	if(ins.operation == "not") {
		emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
		mir_model::Instruction & cmp = emit(mir_model::Instruction::MI_CMP);
		cmp.type = SpellType(ins.type);
		cmp.operands.push_back(MakeReg(reg));
		cmp.operands.push_back(MakeImm(0));
		mir_model::Instruction & set =
			emit(mir_model::Instruction::MI_SETCC);
		set.condition = XC_E;
		set.operands.push_back(MakeReg(reg));
		mir_model::Instruction & widen =
			emit(mir_model::Instruction::MI_MOVZX);
		widen.operands.push_back(MakeReg(reg));
		widen.operands.push_back(MakeReg(reg));
		commit_dest(ins.result);
		return;
	}
	emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
	mir_model::Instruction::Opcode opcode;
	if(ins.operation == "neg")
		opcode = mir_model::Instruction::MI_NEG;
	else if(ins.operation == "bitnot")
		opcode = mir_model::Instruction::MI_NOT;
	else if(ins.operation == "bswap")
		opcode = mir_model::Instruction::MI_BSWAP;
	else
		throw std::runtime_error("unknown unary operation: " + ins.operation);
	mir_model::Instruction & op = emit(opcode);
	op.type = SpellType(ins.type);
	op.operands.push_back(MakeReg(reg));
	emit_narrow_normalize(ins.type, reg);
	commit_dest(ins.result);
}

void FunctionLowering::LowerBinary(const LowIRInstruction & ins)
{
	if(ins.type.is_float()) {
		LowerFloatBinary(ins);
		return;
	}
	if(ins.operation == "div" || ins.operation == "udiv" ||
	   ins.operation == "mod" || ins.operation == "umod") {
		LowerDivision(ins, ins.operation == "mod" ||
		                   ins.operation == "umod");
		return;
	}
	if(ins.operation == "shl" || ins.operation == "shr" ||
	   ins.operation == "ushr") {
		LowerShift(ins);
		return;
	}
	mir_model::Instruction::Opcode opcode;
	if(ins.operation == "add") opcode = mir_model::Instruction::MI_ADD;
	else if(ins.operation == "sub") opcode = mir_model::Instruction::MI_SUB;
	else if(ins.operation == "mul") opcode = mir_model::Instruction::MI_IMUL;
	else if(ins.operation == "and") opcode = mir_model::Instruction::MI_AND;
	else if(ins.operation == "or") opcode = mir_model::Instruction::MI_OR;
	else if(ins.operation == "xor") opcode = mir_model::Instruction::MI_XOR;
	else
		throw std::runtime_error("unknown binary operation: " +
		                         ins.operation);
	const LowIROperand & rhs = ins.operands[1];
	bool rhs_is_lhs = rhs.kind == LOWIR_OPERAND_TEMP &&
	                  ins.operands[0].kind == LOWIR_OPERAND_TEMP &&
	                  rhs.name == ins.operands[0].name;
	if(rhs.kind == LOWIR_OPERAND_TEMP && !rhs_is_lhs &&
	   locations_[rhs.name].kind == ValueLocation::VL_PENDING_COPY)
		resolve_location(rhs.name);
	X64Register reg = XR_RAX;
	emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
	mir_model::Operand source;
	if(rhs_is_lhs) {
		const ValueLocation & location = locations_[rhs.name];
		source = (location.kind == ValueLocation::VL_GPR ||
		          location.kind == ValueLocation::VL_ARG_REG)
			? MakeReg(location.reg) : MakeReg(reg);
	}
	else if(rhs.kind == LOWIR_OPERAND_LITERAL) {
		long long value = ParseIntLiteral(rhs);
		bool bitwise = opcode == mir_model::Instruction::MI_AND ||
		               opcode == mir_model::Instruction::MI_OR ||
		               opcode == mir_model::Instruction::MI_XOR;
		if(FitsImm32(value) && !bitwise) {
			source = MakeImm(value);
		}
		else {
			emit_mov(MakeReg(XR_RDX), MakeImm(value));
			source = MakeReg(XR_RDX);
		}
	}
	else if(rhs.kind == LOWIR_OPERAND_SLOT ||
	        locations_[rhs.name].kind == ValueLocation::VL_FRAME) {
		invalidate_rax();
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = SpellType(ins.type);
		load.operands.push_back(MakeReg(XR_RDX));
		if(rhs.kind == LOWIR_OPERAND_SLOT)
			load.operands.push_back(
				frame_operand(slots_[rhs.name].frame_offset));
		else
			load.operands.push_back(
				frame_operand(locations_[rhs.name].frame_offset));
		source = MakeReg(XR_RDX);
	}
	else {
		source = gpr_read(rhs);
	}
	mir_model::Instruction & op = emit(opcode);
	op.operands.push_back(MakeReg(reg));
	op.operands.push_back(source);
	emit_narrow_normalize(ins.type, reg);
	commit_dest(ins.result);
}

void FunctionLowering::LowerDivision(const LowIRInstruction & ins,
                                     bool modulus)
{
	X64Register reg = XR_RAX;
	emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
	const LowIROperand & rhs = ins.operands[1];
	if(rhs.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(XR_RDX), MakeImm(ParseIntLiteral(rhs)));
		emit_mov(MakeReg(XR_RCX), MakeReg(XR_RDX));
	}
	else {
		emit_mov(MakeReg(XR_RCX), gpr_read(rhs));
	}
	emit_mov(MakeReg(XR_RAX), MakeReg(reg));
	bool sign = ins.operation == "div" || ins.operation == "mod";
	if(sign) {
		emit(mir_model::Instruction::MI_CQO);
	}
	else {
		emit_mov(MakeReg(XR_RDX), MakeImm(0));
	}
	mir_model::Instruction & div =
		emit(sign ? mir_model::Instruction::MI_IDIV
		          : mir_model::Instruction::MI_DIV);
	div.operands.push_back(MakeReg(XR_RCX));
	emit_mov(MakeReg(reg), MakeReg(modulus ? XR_RDX : XR_RAX));
	emit_narrow_normalize(ins.type, reg);
	commit_dest(ins.result);
}

void FunctionLowering::LowerShift(const LowIRInstruction & ins)
{
	X64Register reg = XR_RAX;
	emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
	const LowIROperand & rhs = ins.operands[1];
	if(rhs.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(XR_RDX), MakeImm(ParseIntLiteral(rhs)));
		emit_mov(MakeReg(XR_RCX), MakeReg(XR_RDX));
	}
	else {
		emit_mov(MakeReg(XR_RCX), gpr_read(rhs));
	}
	mir_model::Instruction::Opcode opcode;
	if(ins.operation == "shl")
		opcode = mir_model::Instruction::MI_SHL_CL;
	else if(ins.operation == "ushr" || !type_is_signed(ins.type))
		opcode = mir_model::Instruction::MI_SHR_CL;
	else
		opcode = mir_model::Instruction::MI_SAR_CL;
	mir_model::Instruction & shift = emit(opcode);
	shift.operands.push_back(MakeReg(reg));
	emit_narrow_normalize(ins.type, reg);
	commit_dest(ins.result);
}

void FunctionLowering::LowerCmpValue(const LowIRInstruction & ins)
{
	if(ins.type.is_float()) {
		LowerFloatCmpValue(ins);
		return;
	}
	const LowIROperand & rhs = ins.operands[1];
	bool rhs_is_lhs = rhs.kind == LOWIR_OPERAND_TEMP &&
	                  ins.operands[0].kind == LOWIR_OPERAND_TEMP &&
	                  rhs.name == ins.operands[0].name;
	if(rhs.kind == LOWIR_OPERAND_TEMP && !rhs_is_lhs &&
	   locations_[rhs.name].kind == ValueLocation::VL_PENDING_COPY)
		resolve_location(rhs.name);
	X64Register reg = XR_RAX;
	emit_dest_copy(ins.result, ins.operands[0], ins.type, false, reg);
	mir_model::Operand source;
	if(rhs_is_lhs) {
		const ValueLocation & location = locations_[rhs.name];
		source = (location.kind == ValueLocation::VL_GPR ||
		          location.kind == ValueLocation::VL_ARG_REG)
			? MakeReg(location.reg) : MakeReg(reg);
	}
	else if(rhs.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(XR_RDX), MakeImm(ParseIntLiteral(rhs)));
		source = MakeReg(XR_RDX);
	}
	else if(rhs.kind == LOWIR_OPERAND_SLOT ||
	        locations_[rhs.name].kind == ValueLocation::VL_FRAME) {
		source = MakeReg(reload_to_pool(rhs, values_.count(rhs.name)
			? values_[rhs.name].type : ins.type));
	}
	else {
		source = gpr_read(rhs);
	}
	mir_model::Instruction & cmp = emit(mir_model::Instruction::MI_CMP);
	cmp.type = SpellType(ins.type);
	cmp.operands.push_back(MakeReg(reg));
	cmp.operands.push_back(source);
	mir_model::Instruction & set = emit(mir_model::Instruction::MI_SETCC);
	set.condition = integer_condition(ins.operation,
	                                  type_is_signed(ins.type));
	set.operands.push_back(MakeReg(reg));
	mir_model::Instruction & widen = emit(mir_model::Instruction::MI_MOVZX);
	widen.operands.push_back(MakeReg(reg));
	widen.operands.push_back(MakeReg(reg));
	commit_dest(ins.result);
}

void FunctionLowering::LowerFloatBinary(const LowIRInstruction & ins)
{
	mir_model::Instruction::Opcode opcode;
	if(ins.operation == "add") opcode = mir_model::Instruction::MI_FADD;
	else if(ins.operation == "sub") opcode = mir_model::Instruction::MI_FSUB;
	else if(ins.operation == "mul") opcode = mir_model::Instruction::MI_FMUL;
	else if(ins.operation == "div") opcode = mir_model::Instruction::MI_FDIV;
	else
		throw std::runtime_error("unknown floating binary operation: " +
		                         ins.operation);
	mir_model::Operand a = float_read(ins.operands[0], ins.type);
	mir_model::Operand b = float_read(ins.operands[1], ins.type);
	mir_model::Operand dest;
	if(ins.type.is_f80() || values_[ins.result].crosses_call ||
	   values_[ins.result].needs_frame)
		dest = frame_operand(f80_result_home(ins.result));
	else
		dest = MakeXmm(alloc_xmm(ins.result));
	mir_model::Instruction & op = emit(opcode);
	op.type = SpellType(ins.type);
	op.operands.push_back(dest);
	op.operands.push_back(a);
	op.operands.push_back(b);
}

void FunctionLowering::LowerFloatCmpValue(const LowIRInstruction & ins)
{
	mir_model::Instruction::Opcode opcode;
	if(ins.operation == "eq") opcode = mir_model::Instruction::MI_FEQ;
	else if(ins.operation == "ne") opcode = mir_model::Instruction::MI_FNE;
	else if(ins.operation == "lt" || ins.operation == "ult")
		opcode = mir_model::Instruction::MI_FLT;
	else if(ins.operation == "le" || ins.operation == "ule")
		opcode = mir_model::Instruction::MI_FLE;
	else if(ins.operation == "gt" || ins.operation == "ugt")
		opcode = mir_model::Instruction::MI_FGT;
	else if(ins.operation == "ge" || ins.operation == "uge")
		opcode = mir_model::Instruction::MI_FGE;
	else
		throw std::runtime_error("unknown floating comparison: " +
		                         ins.operation);
	mir_model::Operand a = float_read(ins.operands[0], ins.type);
	mir_model::Operand b = float_read(ins.operands[1], ins.type);
	invalidate_rax();
	mir_model::Instruction & cmp = emit(opcode);
	cmp.type = SpellType(ins.type);
	cmp.operands.push_back(MakeReg(XR_RAX));
	cmp.operands.push_back(a);
	cmp.operands.push_back(b);
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_I64;
	assign_result_from_rax(ins.result, result_type, false);
}

void FunctionLowering::LowerConvert(const LowIRInstruction & ins)
{
	const LowIRType & dst = ins.type;
	const LowIRType & src = ins.type2;
	if(ins.operation == "sext" || ins.operation == "zext" ||
	   ins.operation == "trunc") {
		X64Register reg = XR_RAX;
		emit_dest_copy(ins.result, ins.operands[0], dst, false, reg);
		if(ins.operation == "trunc") {
			emit_narrow_normalize(dst, reg);
			commit_dest(ins.result);
			return;
		}
		mir_model::Instruction & widen =
			emit(ins.operation == "sext"
			         ? mir_model::Instruction::MI_SEXT
			         : mir_model::Instruction::MI_ZEXT);
		widen.type = IntWidthSpelling(src);
		widen.operands.push_back(MakeReg(reg));
		commit_dest(ins.result);
		return;
	}
	if(ins.operation == "sitofp" || ins.operation == "uitofp") {
		mir_model::Instruction::Opcode opcode =
			ins.operation == "sitofp" ? mir_model::Instruction::MI_SITOFP
			                          : mir_model::Instruction::MI_UITOFP;
		mir_model::Operand source;
		if(ins.operands[0].kind == LOWIR_OPERAND_LITERAL)
			source = MakeImm(ParseIntLiteral(ins.operands[0]));
		else
			source = gpr_read(ins.operands[0]);
		mir_model::Operand dest;
		if(dst.is_f80() || values_[ins.result].crosses_call)
			dest = frame_operand(f80_result_home(ins.result));
		else
			dest = MakeXmm(alloc_xmm(ins.result));
		mir_model::Instruction & convert = emit(opcode);
		convert.type = SpellType(dst);
		convert.source_type = IntWidthSpelling(src);
		convert.operands.push_back(dest);
		convert.operands.push_back(source);
		return;
	}
	if(ins.operation == "fptosi" || ins.operation == "fptoui") {
		mir_model::Instruction::Opcode opcode =
			ins.operation == "fptosi" ? mir_model::Instruction::MI_FPTOSI
			                          : mir_model::Instruction::MI_FPTOUI;
		mir_model::Operand source = float_read(ins.operands[0], src);
		X64Register reg = XR_RAX;
		bool param_source = ins.operands[0].kind == LOWIR_OPERAND_TEMP &&
			values_.count(ins.operands[0].name) &&
			values_[ins.operands[0].name].is_param;
		int index = -1;
		if(param_source && !values_[ins.result].crosses_call)
			index = pool_scan(false, true);
		if(index >= 0) {
			reg = kPool[index];
			pool_holder_[index] = ins.result;
			pool_clobbered_[index] = true;
			ValueLocation location;
			location.kind = ValueLocation::VL_GPR;
			location.reg = reg;
			locations_[ins.result] = location;
		}
		else {
			reg = alloc_gpr(ins.result);
		}
		bool frame_result =
			locations_[ins.result].kind == ValueLocation::VL_FRAME;
		if(frame_result) {
			invalidate_rax();
			reg = XR_RAX;
		}
		mir_model::Instruction & convert = emit(opcode);
		convert.type = IntWidthSpelling(dst);
		convert.source_type = SpellType(src);
		convert.operands.push_back(MakeReg(reg));
		convert.operands.push_back(source);
		if(frame_result)
			commit_frame_result(ins.result);
		return;
	}
	if(ins.operation == "fpext" || ins.operation == "fptrunc") {
		mir_model::Instruction::Opcode opcode =
			ins.operation == "fpext" ? mir_model::Instruction::MI_FPEXT
			                         : mir_model::Instruction::MI_FPTRUNC;
		mir_model::Operand source = float_read(ins.operands[0], src);
		mir_model::Operand dest;
		if(dst.is_f80() || values_[ins.result].crosses_call)
			dest = frame_operand(f80_result_home(ins.result));
		else
			dest = MakeXmm(alloc_xmm(ins.result));
		mir_model::Instruction & convert = emit(opcode);
		convert.type = SpellType(dst);
		convert.source_type = SpellType(src);
		convert.operands.push_back(dest);
		convert.operands.push_back(source);
		return;
	}
	throw std::runtime_error("unknown conversion: " + ins.operation);
}

}  // namespace lowir_to_mir
