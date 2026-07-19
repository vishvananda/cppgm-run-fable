#include "x86/lowir_to_mir.h"

#include <stdexcept>

// PA30/PA34 atomic instruction lowering: x86 aligned loads/stores are
// atomic (seq_cst stores publish through xchg); exchange, lock
// cmpxchg, and lock xadd carry the read-modify-write forms.

namespace lowir_to_mir {

void FunctionLowering::LowerAtomicLoad(const LowIRInstruction & ins)
{
	// The copy needs a stable home: LowerLoad defers by pointer.
	rewritten_atomics_.push_back(ins);
	LowIRInstruction & plain = rewritten_atomics_.back();
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

}  // namespace lowir_to_mir
