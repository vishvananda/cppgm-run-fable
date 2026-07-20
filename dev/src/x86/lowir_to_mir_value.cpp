#include "x86/lowir_to_mir.h"

#include <cstdlib>
#include <cstdio>
#include <stdexcept>

// Value, register, and frame machinery for the LowIR -> MIR lowering.
// The pool and staging conventions here are contractual; see
// lowir_to_mir.h.

namespace lowir_to_mir {

const X64Register kArgRegs[6] =
	{ XR_RDI, XR_RSI, XR_RDX, XR_RCX, XR_R8, XR_R9 };
const X64Register kPool[kPoolSize] =
	{ XR_R8, XR_R9, XR_RBX, XR_R12, XR_R13, XR_R14, XR_R15 };
const int kCalleeSavedStart = 2;

namespace {

int pool_index_of(X64Register reg)
{
	for(int i = 0; i < kPoolSize; i++)
		if(kPool[i] == reg)
			return i;
	return -1;
}

}  // namespace

std::string ContainerSpelling(long long bytes)
{
	if(bytes <= 1) return "i8";
	if(bytes <= 2) return "i16";
	if(bytes <= 4) return "i32";
	return "i64";
}

bool FitsImm32(long long value)
{
	return value >= -2147483648LL && value <= 2147483647LL;
}

// Pointer parameters annotated to receive an address rather than a value
// (lowered object arguments, references, indirect results).
bool ParamPassWantsAddress(const LowIRParam & param)
{
	if(param.type.kind != LOWIR_TYPE_PTR)
		return false;
	std::string pass = param.metadata.find("pass");
	return pass == "indirect_result" || pass == "by_address" ||
	       pass == "reference";
}

int ValueInfo::last_use() const
{
	int last = def_position;
	for(size_t i = 0; i < uses.size(); i++)
		if(uses[i].position > last)
			last = uses[i].position;
	return last;
}

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
		case LOWIR_TYPE_I128: return 16;
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

// Allocates a pool register for a fresh value; values whose live range
// crosses a call only take callee-saved registers. Returns false when the
// pool is exhausted (the caller falls back to a frame home).
bool FunctionLowering::alloc_pool_gpr(const std::string & name,
                                      X64Register & out_reg)
{
	// EH mode: values never own pool registers - unwinding into a
	// dispatch block leaves them holding another frame's state, so
	// every value keeps a frame home instead.
	if(eh_mode_)
		return false;
	const ValueInfo & info = values_[name];
	int index = pool_scan(info.crosses_call, false);
	if(index < 0)
		return false;
	pool_holder_[index] = name;
	pool_clobbered_[index] = true;
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
	LowIRType home_type = values_[name].type;
	if(FrameSizeOf(home_type) < 8 && home_type.kind != LOWIR_TYPE_OBJ)
		home_type.kind = LOWIR_TYPE_I64;
	long long offset = alloc_frame_home(name, home_type,
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

mir_model::Operand MakeGot(const std::string & name)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_GOT;
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

mir_model::Operand MakeFloatImm(const LowIROperand & literal,
                                const LowIRType & type)
{
	mir_model::Operand operand;
	operand.kind = mir_model::Operand::OP_FLOAT_IMM;
	operand.text = (literal.negated ? "-" : "") + literal.literal;
	// parse at the operand's own width: rounding twice through long
	// double is one ulp off for literals like 623e+100
	if(type.kind == LOWIR_TYPE_F32)
		operand.float_imm = strtof(operand.text.c_str(), 0);
	else if(type.kind == LOWIR_TYPE_F64)
		operand.float_imm = strtod(operand.text.c_str(), 0);
	else
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
// Scratch parameter copies are assigned at their first read; the copy
// instruction itself is hoisted into the entry-block prologue.
ValueLocation & FunctionLowering::resolve_location(const std::string & name)
{
	ValueLocation & location = locations_[name];
	if(location.kind != ValueLocation::VL_PENDING_COPY)
		return location;
	X64Register home = location.reg;
	// The copy is hoisted to the prologue, so only registers no
	// already-emitted code has written can carry it to this read.
	int index = -1;
	int start = location.pending_r9_first ? 1 : 0;
	for(int i = start; i < kPoolSize && index < 0; i++)
		if(pool_holder_[i].empty() && !pool_clobbered_[i])
			index = i;
	for(int i = 0; i < start && index < 0; i++)
		if(pool_holder_[i].empty() && !pool_clobbered_[i])
			index = i;
	if(index < 0) {
		// out of registers: fall back to a named frame home
		long long offset = alloc_frame_home(
			name, values_[name].type,
			mir_model::FrameBinding::FB_PARAM_SLOT);
		mir_model::Instruction store;
		store.opcode = mir_model::Instruction::MI_STORE;
		store.type = SpellType(values_[name].type);
		store.operands.push_back(frame_operand(offset));
		store.operands.push_back(MakeReg(home));
		out_.blocks[0].instructions.insert(
			out_.blocks[0].instructions.begin() + prologue_length_, store);
		prologue_length_++;
		location.kind = ValueLocation::VL_FRAME;
		location.frame_offset = offset;
		return location;
	}
	X64Register reg = kPool[index];
	pool_holder_[index] = name;
	mir_model::Instruction copy;
	copy.opcode = mir_model::Instruction::MI_MOV;
	copy.operands.push_back(MakeReg(reg));
	copy.operands.push_back(MakeReg(home));
	// hoisted copies keep declaration order, except that a copy never
	// precedes an earlier copy still reading this target register
	int param_index = values_[name].param_index;
	size_t slot = 0;
	while(slot < prologue_entries_.size() &&
	      prologue_entries_[slot].first < param_index)
		slot++;
	for(size_t e = slot; e < prologue_entries_.size(); e++)
		if(prologue_entries_[e].second == (int)reg)
			slot = e + 1;
	size_t base = prologue_length_ - prologue_entries_.size();
	out_.blocks[0].instructions.insert(
		out_.blocks[0].instructions.begin() + base + slot, copy);
	prologue_entries_.insert(prologue_entries_.begin() + slot,
	                         std::make_pair(param_index, (int)home));
	prologue_length_++;
	location.kind = ValueLocation::VL_GPR;
	location.reg = reg;
	location.prefer_home = true;
	return location;
}

mir_model::Operand FunctionLowering::gpr_read(const LowIROperand & operand)
{
	if(operand.kind == LOWIR_OPERAND_TEMP) {
		const ValueLocation & location = resolve_location(operand.name);
		if(location.kind == ValueLocation::VL_GPR ||
		   location.kind == ValueLocation::VL_ARG_REG)
			return MakeReg(location.reg);
		// Spilled and slot-address values reload through the r11/r10
		// staging pair (alternating, so an instruction may read two
		// spilled operands without a collision).
		if(location.kind == ValueLocation::VL_FRAME ||
		   location.kind == ValueLocation::VL_SLOT_ADDR) {
			X64Register staging =
				gpr_read_staging_flip_ ? XR_R10 : XR_R11;
			gpr_read_staging_flip_ = !gpr_read_staging_flip_;
			if(location.kind == ValueLocation::VL_SLOT_ADDR) {
				mir_model::Instruction & lea =
					emit(mir_model::Instruction::MI_LEA);
				lea.operands.push_back(MakeReg(staging));
				lea.operands.push_back(frame_operand(
					slots_[location.slot_name].frame_offset));
				return MakeReg(staging);
			}
			mir_model::Instruction & load =
				emit(mir_model::Instruction::MI_LOAD);
			load.type = "i64";
			load.operands.push_back(MakeReg(staging));
			load.operands.push_back(
				frame_operand(location.frame_offset));
			return MakeReg(staging);
		}
	}
	throw std::logic_error("gpr_read on unhoused operand: " +
	                       operand.name + " in @" + function_.name);
}

// PA36 host data model: a data global the unit only declares takes
// its address from the GOT (host objects spell the access
// R_X86_64_REX_GOTPCRELX; the private linker synthesizes the slot).
// TLS globals keep their wrapper-call path, and functions their
// direct symbol spelling.
bool FunctionLowering::imported_data_global(const std::string & name) const
{
	if(!facts_.host_object)
		return false;
	if(facts_.tls_wrapper_of_global.count(name))
		return false;
	if(facts_.info->is_function(name))
		return false;
	std::map<std::string, const LowIRGlobal *>::const_iterator global =
		facts_.info->globals.find(name);
	return global == facts_.info->globals.end() ||
	       !global->second->is_definition;
}

void FunctionLowering::emit_global_address(X64Register reg,
                                           const std::string & name)
{
	if(imported_data_global(name)) {
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = "ptr";
		load.operands.push_back(MakeReg(reg));
		load.operands.push_back(MakeGot(name));
		return;
	}
	emit_mov(MakeReg(reg), MakeSymbol(name, true));
}

mir_model::Operand FunctionLowering::global_mem_operand(
	const std::string & name, X64Register staging)
{
	if(imported_data_global(name)) {
		mir_model::Instruction & load =
			emit(mir_model::Instruction::MI_LOAD);
		load.type = "ptr";
		load.operands.push_back(MakeReg(staging));
		load.operands.push_back(MakeGot(name));
		return MakeDeref(staging, 0);
	}
	return MakeSymbol(name, true);
}

// Memory operand for a load/store address. `staging` receives lea/load
// materializations for slot-address temps and frame-spilled pointers.
mir_model::Operand FunctionLowering::address_operand(
	const LowIROperand & operand, X64Register staging)
{
	if(operand.kind == LOWIR_OPERAND_GLOBAL)
		return global_mem_operand(operand.name, staging);
	if(operand.kind == LOWIR_OPERAND_SLOT) {
		const SlotInfo & slot = slots_[operand.name];
		return frame_operand(slot.frame_offset);
	}
	if(operand.kind != LOWIR_OPERAND_TEMP)
		throw std::runtime_error("literal used as address: " +
		                         operand.name + "/" + operand.literal +
		                         " in @" + function_.name);
	const ValueLocation & location = resolve_location(operand.name);
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

// Stores a frame-spilled destination computed in rax back to its home.
void FunctionLowering::commit_frame_result(const std::string & dest)
{
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = "i64";
	store.operands.push_back(frame_operand(locations_[dest].frame_offset));
	store.operands.push_back(MakeReg(XR_RAX));
}

void FunctionLowering::commit_dest(const std::string & dest)
{
	if(pending_dest_spill_ == 0)
		return;
	mir_model::Instruction & store = emit(mir_model::Instruction::MI_STORE);
	store.type = "i64";
	store.operands.push_back(frame_operand(pending_dest_spill_));
	store.operands.push_back(MakeReg(XR_RAX));
	pending_dest_spill_ = 0;
	(void)dest;
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
	bool lhs_pending = lhs.kind == LOWIR_OPERAND_TEMP &&
		locations_[lhs.name].kind == ValueLocation::VL_PENDING_COPY;
	bool lhs_forwarded = lhs.kind == LOWIR_OPERAND_TEMP &&
		(locations_[lhs.name].kind == ValueLocation::VL_ARG_REG ||
		 (values_.count(lhs.name) && values_[lhs.name].is_param &&
		  locations_[lhs.name].kind == ValueLocation::VL_FRAME));
	if(lhs.kind == LOWIR_OPERAND_TEMP && !lhs_pending) {
		ValueLocation & location = resolve_location(lhs.name);
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
	if((lhs_forwarded || lhs_pending) && !values_[dest].crosses_call &&
	   !index_dest_lowering_ && !eh_mode_) {
		// destinations fed from parameter registers scan from r9
		int index = pool_scan(false, true);
		if(index < 0)
			index = pool_scan(false, false);
		if(index >= 0) {
			out_reg = kPool[index];
			pool_holder_[index] = dest;
			pool_clobbered_[index] = true;
			ValueLocation location;
			location.kind = ValueLocation::VL_GPR;
			location.reg = out_reg;
			locations_[dest] = location;
		}
		else {
			out_reg = alloc_gpr(dest);
		}
	}
	else {
		out_reg = alloc_gpr(dest);
	}
	if(locations_[dest].kind == ValueLocation::VL_FRAME) {
		// compute in rax, store back after the caller's operation
		invalidate_rax();
		pending_dest_spill_ = locations_[dest].frame_offset;
		out_reg = XR_RAX;
	}
	if(lhs.kind == LOWIR_OPERAND_LITERAL) {
		emit_mov(MakeReg(out_reg), MakeImm(ParseIntLiteral(lhs)));
	}
	else if(lhs.kind == LOWIR_OPERAND_TEMP) {
		const ValueLocation & location = resolve_location(lhs.name);
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
			X64Register source = location.reg;
			const ValueInfo & info = values_[lhs.name];
			if(info.is_param && location.prefer_home &&
			   current_block_ == 0 && !arg_homes_clobbered_) {
				// rdx/rcx double as staging registers, so only the
				// rdi/rsi argument homes are trustworthy this late.
				for(size_t pb = 0; pb < out_.params.size(); pb++)
					if(out_.params[pb].name == lhs.name &&
					   out_.params[pb].location ==
					       mir_model::ParamBinding::PL_REG &&
					   (out_.params[pb].reg == XR_RDI ||
					    out_.params[pb].reg == XR_RSI))
						source = out_.params[pb].reg;
			}
			emit_mov(MakeReg(out_reg), MakeReg(source));
		}
	}
	else if(lhs.kind == LOWIR_OPERAND_GLOBAL) {
		emit_global_address(out_reg, lhs.name);
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
