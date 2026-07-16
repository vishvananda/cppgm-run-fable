#include "x86/lowir_to_mir.h"

#include <stdexcept>

// PA29 128-bit integer lowering. i128 values are always frame-resident
// (16-byte homes, like f80): every operation stages the operand pairs
// through rax:rdx (with rcx/r10/r11 as scratch), computes, and stores
// the destination pair back to its home. Values never own registers,
// so the PA28 register discipline is untouched for everything else.
// Shift counts must be literals; the frontend's division expansion
// (lower_expr.cpp LowerWideDivMod) only shifts by constants.

namespace lowir_to_mir {

bool WideOperandIsI128(const LowIRInstruction & ins)
{
	if (ins.opcode == LOWIR_INS_CALL)
		return false;   // calls stage i128 arguments/results natively
	if (ins.type.kind == LOWIR_TYPE_I128)
		return true;
	return ins.opcode == LOWIR_INS_CONVERT &&
	       ins.type2.kind == LOWIR_TYPE_I128;
}

// The 16-byte frame home of an i128 value (allocated on first sight).
long long FunctionLowering::WideHome(const std::string & name)
{
	ValueLocation & location = locations_[name];
	if (location.kind == ValueLocation::VL_FRAME)
		return location.frame_offset;
	return f80_result_home(name);
}

// Loads one i128 operand's pair into (lo, hi). Literals sign-extend
// their 64-bit spelling.
void FunctionLowering::WideReadPair(const LowIROperand & op,
                                    X64Register lo, X64Register hi)
{
	if (op.kind == LOWIR_OPERAND_LITERAL)
	{
		long long value = ParseIntLiteral(op);
		emit_mov(MakeReg(lo), MakeImm(value));
		emit_mov(MakeReg(hi), MakeImm(value < 0 ? -1 : 0));
		return;
	}
	long long home = 0;
	if (op.kind == LOWIR_OPERAND_SLOT)
		home = slots_[op.name].frame_offset;
	else if (op.kind == LOWIR_OPERAND_TEMP)
		home = WideHome(op.name);
	else
		throw std::runtime_error("unsupported i128 operand form");
	mir_model::Instruction & load_lo = emit(mir_model::Instruction::MI_LOAD);
	load_lo.type = "i64";
	load_lo.operands.push_back(MakeReg(lo));
	load_lo.operands.push_back(frame_operand(home));
	mir_model::Instruction & load_hi = emit(mir_model::Instruction::MI_LOAD);
	load_hi.type = "i64";
	load_hi.operands.push_back(MakeReg(hi));
	mir_model::Operand high = frame_operand(home);
	high.offset += 8;
	load_hi.operands.push_back(high);
}

void FunctionLowering::WideStorePair(long long home, X64Register lo,
                                     X64Register hi)
{
	mir_model::Instruction & store_lo =
		emit(mir_model::Instruction::MI_STORE);
	store_lo.type = "i64";
	store_lo.operands.push_back(frame_operand(home));
	store_lo.operands.push_back(MakeReg(lo));
	mir_model::Instruction & store_hi =
		emit(mir_model::Instruction::MI_STORE);
	store_hi.type = "i64";
	mir_model::Operand high = frame_operand(home);
	high.offset += 8;
	store_hi.operands.push_back(high);
	store_hi.operands.push_back(MakeReg(hi));
}

// add/sub run through the carry pair; the bitwise forms act per half.
void FunctionLowering::LowerWideBinary(const LowIRInstruction & ins)
{
	const std::string & op = ins.operation;
	if (op == "shl" || op == "shr" || op == "ushr")
	{
		LowerWideShift(ins);
		return;
	}
	if (op == "mul")
	{
		LowerWideMul(ins);
		return;
	}
	if (op == "div" || op == "udiv" || op == "mod" || op == "umod")
		throw std::runtime_error(
			"i128 division reaches the backend undecomposed");
	WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
	WideReadPair(ins.operands[1], XR_RCX, XR_R10);
	mir_model::Instruction::Opcode low_op;
	mir_model::Instruction::Opcode high_op;
	if (op == "add")
	{
		low_op = mir_model::Instruction::MI_ADD;
		high_op = mir_model::Instruction::MI_ADC;
	}
	else if (op == "sub")
	{
		low_op = mir_model::Instruction::MI_SUB;
		high_op = mir_model::Instruction::MI_SBB;
	}
	else if (op == "and")
	{
		low_op = high_op = mir_model::Instruction::MI_AND;
	}
	else if (op == "or")
	{
		low_op = high_op = mir_model::Instruction::MI_OR;
	}
	else if (op == "xor")
	{
		low_op = high_op = mir_model::Instruction::MI_XOR;
	}
	else
	{
		throw std::runtime_error("unsupported i128 operation: " + op);
	}
	mir_model::Instruction & low = emit(low_op);
	low.operands.push_back(MakeReg(XR_RAX));
	low.operands.push_back(MakeReg(XR_RCX));
	mir_model::Instruction & high = emit(high_op);
	high.operands.push_back(MakeReg(XR_RDX));
	high.operands.push_back(MakeReg(XR_R10));
	WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
}

// Shifts by a literal count compose from 64-bit shifts of the halves.
void FunctionLowering::LowerWideShift(const LowIRInstruction & ins)
{
	long long count = 0;
	if (ins.operands[1].is_literal())
		count = ParseIntLiteral(ins.operands[1]);
	else if (ins.operands[1].kind == LOWIR_OPERAND_TEMP &&
	         wide_consts_.count(ins.operands[1].name))
		count = wide_consts_[ins.operands[1].name];
	else
		throw std::runtime_error("i128 shift count must be constant");
	count &= 127;
	bool left = ins.operation == "shl";
	bool arith = ins.operation == "shr";
	WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
	mir_model::Instruction::Opcode narrow_shift =
		left ? mir_model::Instruction::MI_SHL_CL
		     : arith ? mir_model::Instruction::MI_SAR_CL
		             : mir_model::Instruction::MI_SHR_CL;
	if (count == 0)
	{
		// the copy still lands in the destination home
	}
	else if (count >= 64)
	{
		if (left)
		{
			// hi = lo << (count - 64); lo = 0
			emit_mov(MakeReg(XR_RDX), MakeReg(XR_RAX));
			if (count > 64)
			{
				emit_mov(MakeReg(XR_RCX), MakeImm(count - 64));
				mir_model::Instruction & shift = emit(narrow_shift);
				shift.operands.push_back(MakeReg(XR_RDX));
			}
			emit_mov(MakeReg(XR_RAX), MakeImm(0));
		}
		else
		{
			// lo = hi >> (count - 64); hi = fill
			emit_mov(MakeReg(XR_RAX), MakeReg(XR_RDX));
			if (count > 64)
			{
				emit_mov(MakeReg(XR_RCX), MakeImm(count - 64));
				mir_model::Instruction & shift = emit(narrow_shift);
				shift.operands.push_back(MakeReg(XR_RAX));
			}
			if (arith)
			{
				emit_mov(MakeReg(XR_RCX), MakeImm(63));
				mir_model::Instruction & fill =
					emit(mir_model::Instruction::MI_SAR_CL);
				fill.operands.push_back(MakeReg(XR_RDX));
			}
			else
			{
				emit_mov(MakeReg(XR_RDX), MakeImm(0));
			}
		}
	}
	else if (left)
	{
		// hi = hi << c | lo >> (64 - c); lo <<= c
		emit_mov(MakeReg(XR_R10), MakeReg(XR_RAX));
		emit_mov(MakeReg(XR_RCX), MakeImm(64 - count));
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_SHR_CL);
		spill.operands.push_back(MakeReg(XR_R10));
		emit_mov(MakeReg(XR_RCX), MakeImm(count));
		mir_model::Instruction & high =
			emit(mir_model::Instruction::MI_SHL_CL);
		high.operands.push_back(MakeReg(XR_RDX));
		mir_model::Instruction & merge =
			emit(mir_model::Instruction::MI_OR);
		merge.operands.push_back(MakeReg(XR_RDX));
		merge.operands.push_back(MakeReg(XR_R10));
		mir_model::Instruction & low =
			emit(mir_model::Instruction::MI_SHL_CL);
		low.operands.push_back(MakeReg(XR_RAX));
	}
	else
	{
		// lo = lo >> c | hi << (64 - c); hi >>= c
		emit_mov(MakeReg(XR_R10), MakeReg(XR_RDX));
		emit_mov(MakeReg(XR_RCX), MakeImm(64 - count));
		mir_model::Instruction & spill =
			emit(mir_model::Instruction::MI_SHL_CL);
		spill.operands.push_back(MakeReg(XR_R10));
		emit_mov(MakeReg(XR_RCX), MakeImm(count));
		mir_model::Instruction & low =
			emit(mir_model::Instruction::MI_SHR_CL);
		low.operands.push_back(MakeReg(XR_RAX));
		mir_model::Instruction & merge =
			emit(mir_model::Instruction::MI_OR);
		merge.operands.push_back(MakeReg(XR_RAX));
		merge.operands.push_back(MakeReg(XR_R10));
		mir_model::Instruction & high = emit(narrow_shift);
		high.operands.push_back(MakeReg(XR_RDX));
	}
	WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
}

// The low 128 bits of the product: lo*lo through the widening one-
// operand mul, plus the truncated cross terms folded into the high
// half (the high*high term falls entirely outside).
void FunctionLowering::LowerWideMul(const LowIRInstruction & ins)
{
	WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
	emit_mov(MakeReg(XR_R10), MakeReg(XR_RAX));   // a.lo
	emit_mov(MakeReg(XR_R11), MakeReg(XR_RDX));   // a.hi
	WideReadPair(ins.operands[1], XR_RCX, XR_RSI);  // b.lo, b.hi
	// rdx:rax = a.lo * b.lo
	emit_mov(MakeReg(XR_RAX), MakeReg(XR_R10));
	mir_model::Instruction & mul = emit(mir_model::Instruction::MI_MUL);
	mul.operands.push_back(MakeReg(XR_RCX));
	// cross terms: a.lo * b.hi + a.hi * b.lo (truncated)
	mir_model::Instruction & cross_a =
		emit(mir_model::Instruction::MI_IMUL);
	cross_a.operands.push_back(MakeReg(XR_R10));
	cross_a.operands.push_back(MakeReg(XR_RSI));
	mir_model::Instruction & cross_b =
		emit(mir_model::Instruction::MI_IMUL);
	cross_b.operands.push_back(MakeReg(XR_R11));
	cross_b.operands.push_back(MakeReg(XR_RCX));
	mir_model::Instruction & fold_a = emit(mir_model::Instruction::MI_ADD);
	fold_a.operands.push_back(MakeReg(XR_RDX));
	fold_a.operands.push_back(MakeReg(XR_R10));
	mir_model::Instruction & fold_b = emit(mir_model::Instruction::MI_ADD);
	fold_b.operands.push_back(MakeReg(XR_RDX));
	fold_b.operands.push_back(MakeReg(XR_R11));
	WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
}

// Pair comparison: subtract-with-borrow for the ordering forms (CF/SF
// carry the 128-bit relation), xor/or for (in)equality.
void FunctionLowering::LowerWideCmp(const LowIRInstruction & ins)
{
	const std::string & pred = ins.operation;
	bool swap = pred == "ugt" || pred == "ule" ||
	            pred == "gt" || pred == "le";
	WideReadPair(ins.operands[swap ? 1 : 0], XR_RAX, XR_RDX);
	WideReadPair(ins.operands[swap ? 0 : 1], XR_RCX, XR_R10);
	X86Condition condition;
	if (pred == "eq" || pred == "ne")
	{
		mir_model::Instruction & low = emit(mir_model::Instruction::MI_XOR);
		low.operands.push_back(MakeReg(XR_RAX));
		low.operands.push_back(MakeReg(XR_RCX));
		mir_model::Instruction & high =
			emit(mir_model::Instruction::MI_XOR);
		high.operands.push_back(MakeReg(XR_RDX));
		high.operands.push_back(MakeReg(XR_R10));
		mir_model::Instruction & fold = emit(mir_model::Instruction::MI_OR);
		fold.operands.push_back(MakeReg(XR_RAX));
		fold.operands.push_back(MakeReg(XR_RDX));
		condition = pred == "eq" ? XC_E : XC_NE;
	}
	else
	{
		// lhs - rhs through the borrow chain; the flags carry the
		// 128-bit relation (unsigned: CF, signed: SF != OF).
		mir_model::Instruction & low = emit(mir_model::Instruction::MI_SUB);
		low.operands.push_back(MakeReg(XR_RAX));
		low.operands.push_back(MakeReg(XR_RCX));
		mir_model::Instruction & high =
			emit(mir_model::Instruction::MI_SBB);
		high.operands.push_back(MakeReg(XR_RDX));
		high.operands.push_back(MakeReg(XR_R10));
		if (pred == "ult" || pred == "ugt")
			condition = XC_B;
		else if (pred == "uge" || pred == "ule")
			condition = XC_AE;
		else if (pred == "lt" || pred == "gt")
			condition = XC_L;
		else if (pred == "ge" || pred == "le")
			condition = XC_GE;
		else
			throw std::runtime_error("unsupported i128 predicate: " +
			                         pred);
	}
	mir_model::Instruction & set = emit(mir_model::Instruction::MI_SETCC);
	set.condition = condition;
	set.operands.push_back(MakeReg(XR_RAX));
	mir_model::Instruction & widen = emit(mir_model::Instruction::MI_MOVZX);
	widen.operands.push_back(MakeReg(XR_RAX));
	widen.operands.push_back(MakeReg(XR_RAX));
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_U8;
	assign_result_from_rax(ins.result, result_type, false);
}

// zext/sext widen through the pair; trunc reads the low half and
// renormalizes at the destination width.
void FunctionLowering::LowerWideConvert(const LowIRInstruction & ins)
{
	bool to_wide = ins.type.kind == LOWIR_TYPE_I128;
	if (to_wide && ins.type2.kind == LOWIR_TYPE_I128)
		throw std::runtime_error("i128 to i128 conversion");
	if (to_wide)
	{
		const LowIROperand & source = ins.operands[0];
		if (source.is_literal())
		{
			// widening a literal (shift counts reach the shifts this
			// way) keeps the known value beside the stored pair; the
			// high half follows the conversion, not the 64-bit sign
			long long value = ParseIntLiteral(source);
			wide_consts_[ins.result] = value;
			emit_mov(MakeReg(XR_RAX), MakeImm(value));
			bool negative = ins.operation == "sext" && value < 0;
			emit_mov(MakeReg(XR_RDX), MakeImm(negative ? -1 : 0));
			WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
			return;
		}
		mir_model::Operand value = gpr_read(source);
		emit_mov(MakeReg(XR_RAX), value);
		if (ins.operation == "sext")
		{
			if (ins.type2.integer_bits() < 64)
			{
				mir_model::Instruction & widen =
					emit(mir_model::Instruction::MI_SEXT);
				widen.type = SpellType(ins.type2);
				widen.operands.push_back(MakeReg(XR_RAX));
			}
			emit_mov(MakeReg(XR_RDX), MakeReg(XR_RAX));
			emit_mov(MakeReg(XR_RCX), MakeImm(63));
			mir_model::Instruction & fill =
				emit(mir_model::Instruction::MI_SAR_CL);
			fill.operands.push_back(MakeReg(XR_RDX));
		}
		else
		{
			if (ins.type2.integer_bits() < 64)
			{
				mir_model::Instruction & widen =
					emit(mir_model::Instruction::MI_ZEXT);
				widen.type = SpellType(ins.type2);
				widen.operands.push_back(MakeReg(XR_RAX));
			}
			emit_mov(MakeReg(XR_RDX), MakeImm(0));
		}
		WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
		return;
	}
	// trunc to 64 bits or narrower: the low half renormalizes
	WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
	invalidate_rax();
	emit_narrow_normalize(ins.type, XR_RAX);
	assign_result_from_rax(ins.result, ins.type, false);
}

void FunctionLowering::LowerWideLoad(const LowIRInstruction & ins)
{
	const LowIROperand & source = ins.operands[0];
	if (source.kind == LOWIR_OPERAND_SLOT ||
	    source.kind == LOWIR_OPERAND_TEMP)
	{
		// pointer temps deref; frame-resident values copy their pair
		if (source.kind == LOWIR_OPERAND_TEMP &&
		    values_.count(source.name) &&
		    values_[source.name].type.kind == LOWIR_TYPE_PTR)
		{
			mir_model::Operand pointer = gpr_read(source);
			emit_mov(MakeReg(XR_RCX), pointer);
			mir_model::Instruction & lo =
				emit(mir_model::Instruction::MI_LOAD);
			lo.type = "i64";
			lo.operands.push_back(MakeReg(XR_RAX));
			lo.operands.push_back(MakeDeref(XR_RCX, 0));
			mir_model::Instruction & hi =
				emit(mir_model::Instruction::MI_LOAD);
			hi.type = "i64";
			hi.operands.push_back(MakeReg(XR_RDX));
			hi.operands.push_back(MakeDeref(XR_RCX, 8));
		}
		else
		{
			WideReadPair(source, XR_RAX, XR_RDX);
		}
		WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
		return;
	}
	throw std::runtime_error("unsupported i128 load form");
}

void FunctionLowering::LowerWideStore(const LowIRInstruction & ins)
{
	WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
	const LowIROperand & target = ins.operands[1];
	if (target.kind == LOWIR_OPERAND_SLOT)
	{
		WideStorePair(slots_[target.name].frame_offset, XR_RAX, XR_RDX);
		return;
	}
	if (target.kind == LOWIR_OPERAND_TEMP)
	{
		mir_model::Operand pointer = gpr_read(target);
		emit_mov(MakeReg(XR_RCX), pointer);
		mir_model::Instruction & lo =
			emit(mir_model::Instruction::MI_STORE);
		lo.type = "i64";
		lo.operands.push_back(MakeDeref(XR_RCX, 0));
		lo.operands.push_back(MakeReg(XR_RAX));
		mir_model::Instruction & hi =
			emit(mir_model::Instruction::MI_STORE);
		hi.type = "i64";
		hi.operands.push_back(MakeDeref(XR_RCX, 8));
		hi.operands.push_back(MakeReg(XR_RDX));
		return;
	}
	throw std::runtime_error("unsupported i128 store form");
}

// The i128 instruction dispatcher: true when this instruction is a
// 128-bit form lowered here (values stay frame-resident throughout).
bool FunctionLowering::LowerWideInstruction(const LowIRInstruction & ins)
{
	if (!WideOperandIsI128(ins))
		return false;
	invalidate_rax();
	switch (ins.opcode)
	{
	case LOWIR_INS_CONST:
	case LOWIR_INS_COPY:
		if (ins.operands[0].is_literal())
			wide_consts_[ins.result] = ParseIntLiteral(ins.operands[0]);
		WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
		WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
		return true;
	case LOWIR_INS_LOAD:
		LowerWideLoad(ins);
		return true;
	case LOWIR_INS_STORE:
		LowerWideStore(ins);
		return true;
	case LOWIR_INS_UNARY:
	{
		if (ins.operation != "neg" && ins.operation != "not")
			throw std::runtime_error("unsupported i128 unary form");
		WideReadPair(ins.operands[0], XR_RCX, XR_R10);
		if (ins.operation == "neg")
		{
			// 0 - x through the borrow pair
			emit_mov(MakeReg(XR_RAX), MakeImm(0));
			emit_mov(MakeReg(XR_RDX), MakeImm(0));
			mir_model::Instruction & low =
				emit(mir_model::Instruction::MI_SUB);
			low.operands.push_back(MakeReg(XR_RAX));
			low.operands.push_back(MakeReg(XR_RCX));
			mir_model::Instruction & high =
				emit(mir_model::Instruction::MI_SBB);
			high.operands.push_back(MakeReg(XR_RDX));
			high.operands.push_back(MakeReg(XR_R10));
		}
		else
		{
			emit_mov(MakeReg(XR_RAX), MakeReg(XR_RCX));
			emit_mov(MakeReg(XR_RDX), MakeReg(XR_R10));
			mir_model::Instruction & low =
				emit(mir_model::Instruction::MI_NOT);
			low.operands.push_back(MakeReg(XR_RAX));
			mir_model::Instruction & high =
				emit(mir_model::Instruction::MI_NOT);
			high.operands.push_back(MakeReg(XR_RDX));
		}
		WideStorePair(WideHome(ins.result), XR_RAX, XR_RDX);
		return true;
	}
	case LOWIR_INS_BINARY:
		LowerWideBinary(ins);
		return true;
	case LOWIR_INS_CMP:
		LowerWideCmp(ins);
		return true;
	case LOWIR_INS_CONVERT:
		LowerWideConvert(ins);
		return true;
	case LOWIR_INS_RETURN:
	{
		WideReadPair(ins.operands[0], XR_RAX, XR_RDX);
		mir_model::Instruction & ret =
			emit(mir_model::Instruction::MI_RET);
		ret.operands.push_back(MakeReg(XR_RAX));
		return true;
	}
	default:
		throw std::runtime_error("unsupported i128 instruction form");
	}
}

}  // namespace lowir_to_mir
