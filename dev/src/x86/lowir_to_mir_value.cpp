#include "x86/lowir_to_mir.h"

#include <cstdlib>
#include <stdexcept>

// Value, register, and frame machinery for the LowIR -> MIR lowering.
// The pool and staging conventions here are contractual; see
// lowir_to_mir.h.

namespace lowir_to_mir {

namespace {

const X64Register kPool[FunctionLowering::kPoolSize] =
	{ XR_R8, XR_R9, XR_RBX, XR_R12, XR_R13, XR_R14, XR_R15 };
const int kCalleeSavedStart = 2;

int pool_index_of(X64Register reg)
{
	for(int i = 0; i < FunctionLowering::kPoolSize; i++)
		if(kPool[i] == reg)
			return i;
	return -1;
}

}  // namespace

long long ParseIntLiteral(const LowIROperand & operand)
{
	if(operand.literal_class == LOWIR_LITERAL_NULLPTR)
		return 0;
	unsigned long long magnitude =
		strtoull(operand.literal.c_str(), 0, 0);
	long long value = (long long)magnitude;
	return operand.negated ? -value : value;
}

// Frame home size and alignment per type (obj payloads round to their
// power-of-two container up to 8, then to 8-byte granules).
long long FrameSizeOf(const LowIRType & type)
{
	switch(type.kind) {
		case LOWIR_TYPE_I1:
		case LOWIR_TYPE_I8:
		case LOWIR_TYPE_U8: return 1;
		case LOWIR_TYPE_I16:
		case LOWIR_TYPE_U16: return 2;
		case LOWIR_TYPE_I32:
		case LOWIR_TYPE_U32: return 4;
		case LOWIR_TYPE_F80: return 16;
		case LOWIR_TYPE_OBJ: {
			long long size = 1;
			while(size < type.obj_bytes && size < 8)
				size *= 2;
			if(type.obj_bytes > 8)
				size = (type.obj_bytes + 7) / 8 * 8;
			return size;
		}
		default: return 8;
	}
}

ValueInfo & FunctionLowering::value(const std::string & name)
{
	return values_[name];
}

bool FunctionLowering::value_pinned(const std::string & name) const
{
	std::map<std::string, ValueInfo>::const_iterator it = values_.find(name);
	if(it == values_.end())
		return false;
	return it->second.is_param || it->second.cross_block;
}

bool FunctionLowering::value_dies_here(const std::string & name) const
{
	std::map<std::string, ValueInfo>::const_iterator it = values_.find(name);
	if(it == values_.end())
		return false;
	return !value_pinned(name) &&
	       it->second.last_use() <= current_position_;
}

bool FunctionLowering::pool_reg_free(int pool_index) const
{
	return pool_holder_[pool_index].empty();
}

int FunctionLowering::pool_scan(bool callee_saved_only,
                                bool skip_scratch) const
{
	int start = (callee_saved_only || skip_scratch) ? kCalleeSavedStart : 0;
	if(skip_scratch && !callee_saved_only)
		start = 1;
	for(int i = start; i < kPoolSize; i++)
		if(pool_holder_[i].empty())
			return i;
	return -1;
}

void FunctionLowering::note_callee_saved(X64Register reg)
{
	for(size_t i = 0; i < preserve_.size(); i++)
		if(preserve_[i] == reg)
			return;
	preserve_.push_back(reg);
}

// Allocates a pool register for a fresh value; values whose live range
// crosses a call only take callee-saved registers. Returns false when the
// pool is exhausted (the caller falls back to a frame home).
bool FunctionLowering::alloc_pool_gpr(const std::string & name,
                                      X64Register & out_reg)
{
	const ValueInfo & info = values_[name];
	int index = pool_scan(info.crosses_call, false);
	if(index < 0)
		return false;
	pool_holder_[index] = name;
	if(index >= kCalleeSavedStart)
		note_callee_saved(kPool[index]);
	ValueLocation location;
	location.kind = ValueLocation::VL_GPR;
	location.reg = kPool[index];
	locations_[name] = location;
	out_reg = kPool[index];
	return true;
}

X64Register FunctionLowering::alloc_gpr(const std::string & name)
{
	X64Register reg = XR_RAX;
	if(alloc_pool_gpr(name, reg))
		return reg;
	long long offset = alloc_frame_home(name, values_[name].type,
	                                    mir_model::FrameBinding::FB_TEMP);
	ValueLocation location;
	location.kind = ValueLocation::VL_FRAME;
	location.frame_offset = offset;
	locations_[name] = location;
	return XR_RAX;
}

XmmRegister FunctionLowering::alloc_xmm(const std::string & name)
{
	for(int i = 0; i < 8; i++) {
		if(xmm_holder_[i].empty()) {
			xmm_holder_[i] = name;
			ValueLocation location;
			location.kind = ValueLocation::VL_XMM;
			location.xmm = (XmmRegister)i;
			locations_[name] = location;
			return (XmmRegister)i;
		}
	}
	throw std::runtime_error("xmm register pressure not supported");
}

long long FunctionLowering::alloc_frame_home(
	const std::string & name, const LowIRType & type,
	mir_model::FrameBinding::Kind kind)
{
	long long size = FrameSizeOf(type);
	long long align = size < 8 ? size : 8;
	if(type.kind == LOWIR_TYPE_F80)
		align = 16;
	frame_cursor_ = (frame_cursor_ + size + align - 1) / align * align;
	mir_model::FrameBinding binding;
	binding.kind = kind;
	binding.name = name;
	binding.offset = -frame_cursor_;
	binding.type = SpellType(type);
	out_.frame_bindings.push_back(binding);
	return -frame_cursor_;
}

long long FunctionLowering::alloc_anonymous_spill()
{
	frame_cursor_ = (frame_cursor_ + 8 + 7) / 8 * 8;
	return -frame_cursor_;
}

void FunctionLowering::release_value(const std::string & name)
{
	std::map<std::string, ValueLocation>::iterator it =
		locations_.find(name);
	if(it == locations_.end())
		return;
	for(int i = 0; i < kPoolSize; i++)
		if(pool_holder_[i] == name)
			pool_holder_[i] = "";
	if(it->second.kind == ValueLocation::VL_XMM &&
	   xmm_holder_[it->second.xmm] == name)
		xmm_holder_[it->second.xmm] = "";
	if(rax_alias_ == name)
		rax_alias_ = "";
	it->second.kind = ValueLocation::VL_NONE;
}

void FunctionLowering::release_after_use(const LowIRInstruction & ins)
{
	std::vector<const LowIROperand *> reads;
	for(size_t o = 0; o < ins.operands.size(); o++)
		reads.push_back(&ins.operands[o]);
	for(size_t v = 0; v < ins.switch_values.size(); v++)
		reads.push_back(&ins.switch_values[v]);
	for(size_t r = 0; r < reads.size(); r++) {
		if(reads[r]->kind != LOWIR_OPERAND_TEMP)
			continue;
		if(value_dies_here(reads[r]->name))
			release_value(reads[r]->name);
	}
	if(ins.callee_is_temp && value_dies_here(ins.callee))
		release_value(ins.callee);
}

void FunctionLowering::invalidate_rax()
{
	if(rax_alias_.empty())
		return;
	std::map<std::string, ValueLocation>::iterator it =
		locations_.find(rax_alias_);
	if(it != locations_.end())
		it->second.also_in_rax = false;
	rax_alias_ = "";
}

bool FunctionLowering::operand_in_rax(const LowIROperand & operand) const
{
	if(operand.kind != LOWIR_OPERAND_TEMP || rax_alias_.empty())
		return false;
	return operand.name == rax_alias_;
}

mir_model::Instruction & FunctionLowering::emit(
	mir_model::Instruction::Opcode opcode)
{
	mir_model::Instruction instruction;
	instruction.opcode = opcode;
	mir_block_->instructions.push_back(instruction);
	return mir_block_->instructions.back();
}

void FunctionLowering::emit_mov(const mir_model::Operand & dst,
                                const mir_model::Operand & src)
{
	mir_model::Instruction & mov = emit(mir_model::Instruction::MI_MOV);
	mov.operands.push_back(dst);
	mov.operands.push_back(src);
	if(dst.kind == mir_model::Operand::OP_REG && dst.reg == XR_RAX)
		invalidate_rax();
}

mir_model::Operand MakeReg(X64Register reg)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_REG;
	operand.reg = reg;
	return operand;
}

mir_model::Operand MakeXmm(XmmRegister xmm)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_XMM;
	operand.xmm = xmm;
	return operand;
}

mir_model::Operand MakeImm(long long value)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_IMM;
	operand.imm = value;
	return operand;
}

mir_model::Operand MakeSymbol(const std::string & name, bool global)
{
	mir_model::Operand operand;
	operand.kind = global ? mir_model::Operand::OP_GLOBAL
	                      : mir_model::Operand::OP_SYMBOL;
	operand.text = name;
	return operand;
}

mir_model::Operand MakeLabel(const std::string & label)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_LABEL;
	operand.text = label;
	return operand;
}

mir_model::Operand MakeDeref(X64Register reg, long long offset)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_DEREF;
	operand.reg = reg;
	operand.offset = offset;
	return operand;
}

mir_model::Operand MakeFloatImm(const LowIROperand & literal)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_FLOAT_IMM;
	operand.text = (literal.negated ? "-" : "") + literal.literal;
	operand.float_imm = strtold(operand.text.c_str(), 0);
	return operand;
}

mir_model::Operand FunctionLowering::frame_operand(long long offset) const
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_FRAME;
	operand.offset = offset;
	return operand;
}

// Reads an integer-class operand that is already in a register (or is a
// forwarded parameter). The caller handles literals, frame homes, and
// pending loads before calling this.
mir_model::Operand FunctionLowering::gpr_read(const LowIROperand & operand)
{
	if(operand.kind == LOWIR_OPERAND_TEMP) {
		const ValueLocation & location = locations_[operand.name];
		if(location.kind == ValueLocation::VL_GPR ||
		   location.kind == ValueLocation::VL_ARG_REG)
			return MakeReg(location.reg);
	}
	throw std::logic_error("gpr_read on unhoused operand");
}

// Memory operand for a load/store address. `staging` receives lea/load
// materializations for slot-address temps and frame-spilled pointers.
mir_model::Operand FunctionLowering::address_operand(
	const LowIROperand & operand, X64Register staging)
{
	if(operand.kind == LOWIR_OPERAND_GLOBAL)
		return MakeSymbol(operand.name, true);
	if(operand.kind == LOWIR_OPERAND_SLOT) {
		const SlotInfo & slot = slots_[operand.name];
		return frame_operand(slot.frame_offset);
	}
	if(operand.kind != LOWIR_OPERAND_TEMP)
		throw std::runtime_error("literal used as address");
	const ValueLocation & location = locations_[operand.name];
	if(location.kind == ValueLocation::VL_SLOT_ADDR) {
		mir_model::Instruction & lea = emit(mir_model::Instruction::MI_LEA);
		lea.operands.push_back(MakeReg(staging));
		lea.operands.push_back(
			frame_operand(slots_[location.slot_name].frame_offset));
		return MakeDeref(staging, 0);
	}
	if(location.kind == ValueLocation::VL_FRAME) {
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = "ptr";
		load.operands.push_back(MakeReg(staging));
		load.operands.push_back(frame_operand(location.frame_offset));
		return MakeDeref(staging, 0);
	}
	if(location.kind == ValueLocation::VL_GPR ||
	   location.kind == ValueLocation::VL_ARG_REG)
		return MakeDeref(location.reg, 0);
	throw std::logic_error("address operand without a location");
}

// Transient reload of a frame-resident value (spilled temp or slot
// content) into the next free pool register, re-normalized at its own
// width; the register is released again by the caller's instruction.
X64Register FunctionLowering::reload_to_pool(const LowIROperand & operand,
                                             const LowIRType & type)
{
	int index = pool_scan(false, false);
	X64Register reg = XR_RDX;
	if(index >= 0) {
		reg = kPool[index];
		if(index >= kCalleeSavedStart)
			note_callee_saved(reg);
	}
	long long offset;
	if(operand.kind == LOWIR_OPERAND_SLOT)
		offset = slots_[operand.name].frame_offset;
	else
		offset = locations_[operand.name].frame_offset;
	mir_model::Instruction & load = emit(mir_model::Instruction::MI_LOAD);
	load.type = SpellType(type);
	load.operands.push_back(MakeReg(reg));
	load.operands.push_back(frame_operand(offset));
	emit_narrow_normalize(type, reg);
	return reg;
}

void FunctionLowering::emit_narrow_normalize(const LowIRType & type,
                                             X64Register reg)
{
	bool sign;
	std::string width;
	switch(type.kind) {
		case LOWIR_TYPE_I8: sign = true; width = "i8"; break;
		case LOWIR_TYPE_I16: sign = true; width = "i16"; break;
		case LOWIR_TYPE_I32: sign = true; width = "i32"; break;
		case LOWIR_TYPE_I1: sign = false; width = "i1"; break;
		case LOWIR_TYPE_U8: sign = false; width = "i8"; break;
		case LOWIR_TYPE_U16: sign = false; width = "i16"; break;
		case LOWIR_TYPE_U32: sign = false; width = "i32"; break;
		default: return;
	}
	mir_model::Instruction & norm =
		emit(sign ? mir_model::Instruction::MI_SEXT
		          : mir_model::Instruction::MI_ZEXT);
	norm.type = width;
	norm.operands.push_back(MakeReg(reg));
}

// The shared `mov dest, lhs` opening of binary/cmp/unary templates.
// Reuses a dying temp's register in place; otherwise allocates the
// destination (while the operands are still live) and copies.
void FunctionLowering::emit_dest_copy(const std::string & dest,
                                      const LowIROperand & lhs,
                                      const LowIRType & type,
                                      bool normalize,
                                      X64Register & out_reg)
{
	if(lhs.kind == LOWIR_OPERAND_TEMP) {
		ValueLocation & location = locations_[lhs.name];
		bool class_ok =
			!values_[dest].crosses_call ||
			pool_index_of(location.reg) >= kCalleeSavedStart;
		if(location.kind == ValueLocation::VL_GPR &&
		   value_dies_here(lhs.name) && class_ok) {
			ValueLocation moved = location;
			moved.also_in_rax = false;
			int index = pool_index_of(location.reg);
			release_value(lhs.name);
			pool_holder_[index] = dest;
			locations_[dest] = moved;
			out_reg = moved.reg;
			if(normalize)
				emit_narrow_normalize(type, out_reg);
			return;
		}
	}
	out_reg = alloc_gpr(dest);
	if(locations_[dest].kind == ValueLocation::VL_FRAME)
		throw std::runtime_error("integer register pressure spill at def");
	if(lhs.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(out_reg), MakeImm(ParseIntLiteral(lhs)));
	}
	else if(lhs.kind == LOWIR_OPERAND_TEMP) {
		const ValueLocation & location = locations_[lhs.name];
		if(location.kind == ValueLocation::VL_FRAME) {
			mir_model::Instruction & load =
				emit(mir_model::Instruction::MI_LOAD);
			load.type = SpellType(type);
			load.operands.push_back(MakeReg(out_reg));
			load.operands.push_back(frame_operand(location.frame_offset));
		}
		else if(location.kind == ValueLocation::VL_SLOT_ADDR) {
			mir_model::Instruction & lea =
				emit(mir_model::Instruction::MI_LEA);
			lea.operands.push_back(MakeReg(out_reg));
			lea.operands.push_back(
				frame_operand(slots_[location.slot_name].frame_offset));
		}
		else {
			emit_mov(MakeReg(out_reg), MakeReg(location.reg));
		}
	}
	else if(lhs.kind == LOWIR_OPERAND_GLOBAL) {
		emit_mov(MakeReg(out_reg), MakeSymbol(lhs.name, true));
	}
	else if(lhs.kind == LOWIR_OPERAND_SLOT) {
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = SpellType(type);
		load.operands.push_back(MakeReg(out_reg));
		load.operands.push_back(
			frame_operand(slots_[lhs.name].frame_offset));
	}
	else {
		throw std::runtime_error("unsupported dest-copy operand");
	}
	if(normalize)
		emit_narrow_normalize(type, out_reg);
}

}  // namespace lowir_to_mir
