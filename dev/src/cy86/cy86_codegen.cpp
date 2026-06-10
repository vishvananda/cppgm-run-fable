#include "cy86/cy86_codegen.h"

#include <stdexcept>

using std::logic_error;

// Translation conventions (the handout-recommended design):
//
//   cy86  sp bp x64 y64 z64 t64
//   x86   rsp rbp r12 r13 r14 r15
//
// Per instruction, source operands load into rax/rbx/rcx, memory
// addresses compute in rsi (loads) and rdi (stores), and rdx is
// implicitly used by mul/div. The red zone holds per-instruction
// temporaries: [rsp-8..rsp-56] stage system call arguments,
// [rsp-80..rsp-65] is the x87 transfer slot, [rsp-96..rsp-89] the
// fistp result slot. CY86 programs may not touch the red zone, so
// nothing here is live across instructions.

namespace {

const int kSlotFloat = -80;
const int kSlotInt = -96;

int MapRegister(ECY86Reg reg)
{
	switch (reg)
	{
	case CY86_REG_SP: return X86_RSP;
	case CY86_REG_BP: return X86_RBP;
	case CY86_REG_X: return X86_R12;
	case CY86_REG_Y: return X86_R13;
	case CY86_REG_Z: return X86_R14;
	case CY86_REG_T: return X86_R15;
	}
	throw logic_error("unmapped CY86 register");
}

// The low 64 bits of an immediate or address value: literal operand
// immediates carry converted bytes, memory address terms a 64-bit
// addend.
X86Imm ImmValue(const CY86Immediate& imm)
{
	if (imm.has_label)
		return X86Imm::Label(imm.label, imm.addend);
	if (!imm.bytes.empty())
		return X86Imm::Constant(LittleEndianValue(imm.bytes.substr(0, 8)));
	return X86Imm::Constant(imm.addend);
}

// Bits 64..79 of an 80-bit immediate (a label value zero-extends).
X86Imm Imm80High(const CY86Immediate& imm)
{
	if (imm.has_label)
		return X86Imm::Constant(0);
	return X86Imm::Constant(LittleEndianValue(imm.bytes.substr(8)));
}

int RelationCond(int variant)
{
	bool is_unsigned = (variant & CY86_REL_UNSIGNED) != 0;
	switch (variant & ~CY86_REL_UNSIGNED)
	{
	case CY86_REL_EQ: return X86_CC_E;
	case CY86_REL_NE: return X86_CC_NE;
	case CY86_REL_LT: return is_unsigned ? X86_CC_B : X86_CC_L;
	case CY86_REL_GT: return is_unsigned ? X86_CC_A : X86_CC_G;
	case CY86_REL_LE: return is_unsigned ? X86_CC_BE : X86_CC_LE;
	case CY86_REL_GE: return is_unsigned ? X86_CC_AE : X86_CC_GE;
	}
	throw logic_error("unmapped CY86 comparison relation");
}

class StatementTranslator
{
public:
	explicit StatementTranslator(const CY86Statement& stmt) : stmt_(stmt)
	{}

	void Translate(ImageItem& item)
	{
		EmitInstruction();
		item.bytes = code_.bytes();
		item.patches = code_.patches();
	}

private:
	const CY86Operand& Operand(size_t i) const
	{
		return stmt_.operands[i];
	}

	int OperandWidth(size_t i) const
	{
		return stmt_.opcode->operands[i].width;
	}

	// Address of a memory operand into `reg`.
	void LoadAddress(const CY86Operand& op, int reg)
	{
		if (!op.mem_has_base)
		{
			code_.MovRegImm(reg, ImmValue(op.imm));
			return;
		}
		if (!op.imm.has_label && op.imm.addend == 0)
		{
			code_.MovRegReg(64, reg, MapRegister(op.mem_base));
			return;
		}
		code_.MovRegImm(reg, ImmValue(op.imm));
		code_.Alu(X86_ADD, 64, reg, MapRegister(op.mem_base));
	}

	// Source operand value (low `width` bits) into `reg`.
	void LoadSource(const CY86Operand& op, int width, int reg)
	{
		switch (op.kind)
		{
		case CY86_OPERAND_REGISTER:
			code_.MovRegReg(width, reg, MapRegister(op.reg));
			break;
		case CY86_OPERAND_IMMEDIATE:
			code_.MovRegImm(reg, ImmValue(op.imm));
			break;
		case CY86_OPERAND_MEMORY:
			LoadAddress(op, X86_RSI);
			code_.MovRegMem(width, reg, X86Mem(X86_RSI, 0));
			break;
		}
	}

	// Source operand into rax, extended to a full 64-bit value.
	void LoadSourceExtended(const CY86Operand& op, int width,
	                        bool is_signed)
	{
		LoadSource(op, width, X86_RAX);
		if (is_signed)
		{
			if (width < 64)
				code_.Movsx64(width, X86_RAX, X86_RAX);
		}
		else if (width <= 16)
		{
			code_.Movzx(width, X86_RAX, X86_RAX);
		}
		else if (width == 32)
		{
			code_.MovRegReg(32, X86_RAX, X86_RAX);
		}
	}

	void StoreResult(const CY86Operand& op, int width, int reg)
	{
		if (op.kind == CY86_OPERAND_REGISTER)
		{
			code_.MovRegReg(width, MapRegister(op.reg), reg);
			return;
		}
		LoadAddress(op, X86_RDI);
		code_.MovMemReg(width, X86Mem(X86_RDI, 0), reg);
	}

	// Push a floating source operand onto the x87 stack.
	void PushFloat(const CY86Operand& op, int width)
	{
		if (op.kind == CY86_OPERAND_MEMORY)
		{
			LoadAddress(op, X86_RSI);
			code_.X87Mem(X86_FLD, width, X86Mem(X86_RSI, 0));
			return;
		}
		if (width == 80)
		{
			// Immediate: no 80-bit registers exist. Stage all ten bytes.
			code_.MovRegImm(X86_RAX, ImmValue(op.imm));
			code_.MovMemReg(64, X86Mem(X86_RSP, kSlotFloat), X86_RAX);
			code_.MovRegImm(X86_RAX, Imm80High(op.imm));
			code_.MovMemReg(16, X86Mem(X86_RSP, kSlotFloat + 8), X86_RAX);
			code_.X87Mem(X86_FLD, 80, X86Mem(X86_RSP, kSlotFloat));
			return;
		}
		LoadSource(op, width, X86_RAX);
		code_.MovMemReg(width, X86Mem(X86_RSP, kSlotFloat), X86_RAX);
		code_.X87Mem(X86_FLD, width, X86Mem(X86_RSP, kSlotFloat));
	}

	// Pop st(0) into the written operand.
	void StoreFloat(const CY86Operand& op, int width)
	{
		if (op.kind == CY86_OPERAND_MEMORY)
		{
			LoadAddress(op, X86_RDI);
			code_.X87Mem(X86_FSTP, width, X86Mem(X86_RDI, 0));
			return;
		}
		code_.X87Mem(X86_FSTP, width, X86Mem(X86_RSP, kSlotFloat));
		code_.MovRegMem(width, X86_RAX, X86Mem(X86_RSP, kSlotFloat));
		code_.MovRegReg(width, MapRegister(op.reg), X86_RAX);
	}

	void EmitMove()
	{
		int width = OperandWidth(0);
		if (width == 80)
		{
			EmitMove80();
			return;
		}
		LoadSource(Operand(1), width, X86_RAX);
		StoreResult(Operand(0), width, X86_RAX);
	}

	void EmitMove80()
	{
		const CY86Operand& src = Operand(1);
		if (src.kind == CY86_OPERAND_MEMORY)
		{
			LoadAddress(src, X86_RSI);
			code_.MovRegMem(64, X86_RAX, X86Mem(X86_RSI, 0));
			code_.MovRegMem(16, X86_RCX, X86Mem(X86_RSI, 8));
		}
		else
		{
			code_.MovRegImm(X86_RAX, ImmValue(src.imm));
			code_.MovRegImm(X86_RCX, Imm80High(src.imm));
		}
		LoadAddress(Operand(0), X86_RDI);
		code_.MovMemReg(64, X86Mem(X86_RDI, 0), X86_RAX);
		code_.MovMemReg(16, X86Mem(X86_RDI, 8), X86_RCX);
	}

	void EmitJumpIf()
	{
		LoadSource(Operand(0), 8, X86_RAX);
		LoadSource(Operand(1), 64, X86_RBX);
		code_.Alu(X86_TEST, 8, X86_RAX, X86_RAX);
		X86CodeBuffer target;
		target.JmpReg(X86_RBX);
		code_.JccRel8(X86_CC_E, static_cast<int>(target.bytes().size()));
		code_.AppendBuffer(target);
	}

	// and/or/xor/iadd/isub: a width-exact two-register ALU operation.
	void EmitBinary(EX86Mnemonic mnemonic)
	{
		int width = OperandWidth(0);
		LoadSource(Operand(1), width, X86_RAX);
		LoadSource(Operand(2), width, X86_RBX);
		code_.Alu(mnemonic, width, X86_RAX, X86_RBX);
		StoreResult(Operand(0), width, X86_RAX);
	}

	void EmitShift(EX86Mnemonic mnemonic)
	{
		int width = OperandWidth(0);
		LoadSource(Operand(1), width, X86_RAX);
		LoadSource(Operand(2), 8, X86_RCX);
		code_.ShiftRegCl(mnemonic, width, X86_RAX);
		StoreResult(Operand(0), width, X86_RAX);
	}

	void EmitMul(int variant)
	{
		int width = OperandWidth(0);
		LoadSource(Operand(1), width, X86_RAX);
		LoadSource(Operand(2), width, X86_RBX);
		code_.MulDiv(variant == 0 ? X86_IMUL : X86_MUL, width, X86_RBX);
		StoreResult(Operand(0), width, X86_RAX);
	}

	void EmitDiv(int variant)
	{
		bool is_signed = variant == 0 || variant == 2;
		bool is_mod = variant >= 2;
		int width = OperandWidth(0);
		LoadSource(Operand(1), width, X86_RAX);
		LoadSource(Operand(2), width, X86_RBX);
		if (is_signed)
		{
			code_.SignExtendAcc(width);
			code_.MulDiv(X86_IDIV, width, X86_RBX);
		}
		else
		{
			// Zero the high half of the dividend (ah / rdx).
			if (width == 8)
				code_.Movzx(8, X86_RAX, X86_RAX);
			else
				code_.Alu(X86_XOR, 32, X86_RDX, X86_RDX);
			code_.MulDiv(X86_DIV, width, X86_RBX);
		}
		if (is_mod)
		{
			if (width == 8)
				code_.ShrRegImm(16, X86_RAX, 8);  // remainder is in ah
			else
				code_.MovRegReg(width, X86_RAX, X86_RDX);
		}
		StoreResult(Operand(0), width, X86_RAX);
	}

	void EmitIntCompare(int variant)
	{
		int width = OperandWidth(1);
		LoadSource(Operand(1), width, X86_RAX);
		LoadSource(Operand(2), width, X86_RBX);
		code_.Alu(X86_CMP, width, X86_RAX, X86_RBX);
		code_.Setcc(RelationCond(variant), X86_RAX);
		StoreResult(Operand(0), 8, X86_RAX);
	}

	void EmitFloatCompare(int variant)
	{
		int width = OperandWidth(1);
		PushFloat(Operand(2), width);
		PushFloat(Operand(1), width);  // st(0)=op2, st(1)=op3
		code_.X87Stack(X86_FCOMIP);    // sets CF/ZF like unsigned cmp
		code_.X87Stack(X86_FSTP_ST0);
		code_.Setcc(RelationCond(variant | CY86_REL_UNSIGNED), X86_RAX);
		StoreResult(Operand(0), 8, X86_RAX);
	}

	void EmitFloatArith(int variant)
	{
		int width = OperandWidth(0);
		PushFloat(Operand(1), width);
		PushFloat(Operand(2), width);  // st(0)=op3, st(1)=op2
		EX86Mnemonic mnemonic =
			variant == 0 ? X86_FADDP
			: variant == 1 ? X86_FSUBP
			: variant == 2 ? X86_FMULP : X86_FDIVP;
		code_.X87Stack(mnemonic);      // st(1) op= st(0); pop
		StoreFloat(Operand(0), width);
	}

	void EmitToF80(int variant)
	{
		int width = OperandWidth(1);
		if (variant == 2)
		{
			PushFloat(Operand(1), width);
			StoreFloat(Operand(0), 80);
			return;
		}
		LoadSourceExtended(Operand(1), width, variant == 0);
		code_.MovMemReg(64, X86Mem(X86_RSP, kSlotFloat), X86_RAX);
		code_.X87Mem(X86_FILD, 64, X86Mem(X86_RSP, kSlotFloat));
		if (variant == 1 && width == 64)
		{
			// fild treats the slot as signed; values with the top bit
			// set converted 2^64 too low. Compensate when rax < 0.
			code_.Alu(X86_TEST, 64, X86_RAX, X86_RAX);
			X86CodeBuffer fix;
			fix.MovRegImm(X86_RCX,
			              X86Imm::Constant(0x43F0000000000000ull));
			fix.MovMemReg(64, X86Mem(X86_RSP, kSlotFloat), X86_RCX);
			fix.X87Mem(X86_FADD_M64, 64, X86Mem(X86_RSP, kSlotFloat));
			code_.JccRel8(X86_CC_NS,
			              static_cast<int>(fix.bytes().size()));
			code_.AppendBuffer(fix);
		}
		StoreFloat(Operand(0), 80);
	}

	void EmitFromF80(int variant)
	{
		int width = OperandWidth(0);
		PushFloat(Operand(1), 80);
		if (variant == 2)
		{
			StoreFloat(Operand(0), width);
			return;
		}
		if (variant == 1 && width == 64)
		{
			EmitF80ToU64();
			StoreResult(Operand(0), 64, X86_RAX);
			return;
		}
		// fistp has no 8-bit form and is signed: round-trip through a
		// width that holds the whole value range (exactness is the
		// program's obligation).
		int fist_width = variant == 0 ? (width == 8 ? 16 : width)
		                              : (width == 8 ? 16
		                                 : width == 16 ? 32 : 64);
		code_.X87Mem(X86_FISTP, fist_width, X86Mem(X86_RSP, kSlotInt));
		code_.MovRegMem(64, X86_RAX, X86Mem(X86_RSP, kSlotInt));
		StoreResult(Operand(0), width, X86_RAX);
	}

	// st(0) holds the value; result in rax. Values of 2^63 and above
	// exceed fistp's signed range: convert v-2^63 and add 2^63 back.
	void EmitF80ToU64()
	{
		code_.MovRegImm(X86_RCX, X86Imm::Constant(0x43E0000000000000ull));
		code_.MovMemReg(64, X86Mem(X86_RSP, kSlotFloat), X86_RCX);
		code_.X87Mem(X86_FLD, 64, X86Mem(X86_RSP, kSlotFloat));
		code_.X87Stack(X86_FCOMIP);  // compares 2^63 with v; pops
		X86CodeBuffer small;
		small.X87Mem(X86_FISTP, 64, X86Mem(X86_RSP, kSlotInt));
		small.MovRegMem(64, X86_RAX, X86Mem(X86_RSP, kSlotInt));
		X86CodeBuffer big;
		big.X87Mem(X86_FSUB_M64, 64, X86Mem(X86_RSP, kSlotFloat));
		big.X87Mem(X86_FISTP, 64, X86Mem(X86_RSP, kSlotInt));
		big.MovRegMem(64, X86_RAX, X86Mem(X86_RSP, kSlotInt));
		big.MovRegImm(X86_RCX, X86Imm::Constant(0x8000000000000000ull));
		big.Alu(X86_ADD, 64, X86_RAX, X86_RCX);
		big.JmpRel8(static_cast<int>(small.bytes().size()));
		code_.JccRel8(X86_CC_A, static_cast<int>(big.bytes().size()));
		code_.AppendBuffer(big);
		code_.AppendBuffer(small);
	}

	void EmitSyscall(int param_count)
	{
		static const int kArgRegs[7] =
		{
			X86_RAX, X86_RDI, X86_RSI, X86_RDX, X86_R10, X86_R8, X86_R9
		};
		int sources = param_count + 1;  // the call number, then params
		for (int i = 0; i < sources; i++)
		{
			LoadSource(Operand(static_cast<size_t>(i) + 1), 64, X86_RAX);
			code_.MovMemReg(64, X86Mem(X86_RSP, -8 * (i + 1)), X86_RAX);
		}
		for (int i = 0; i < sources; i++)
			code_.MovRegMem(64, kArgRegs[i],
			                X86Mem(X86_RSP, -8 * (i + 1)));
		code_.Syscall();
		StoreResult(Operand(0), 64, X86_RAX);
	}

	void EmitInstruction()
	{
		const CY86Opcode& opcode = *stmt_.opcode;
		switch (opcode.family)
		{
		case CY86_FAM_MOVE:
			EmitMove();
			break;
		case CY86_FAM_JUMP:
			LoadSource(Operand(0), 64, X86_RAX);
			code_.JmpReg(X86_RAX);
			break;
		case CY86_FAM_JUMPIF:
			EmitJumpIf();
			break;
		case CY86_FAM_CALL:
			LoadSource(Operand(0), 64, X86_RAX);
			code_.CallReg(X86_RAX);
			break;
		case CY86_FAM_RET:
			code_.Ret();
			break;
		case CY86_FAM_NOT:
			LoadSource(Operand(1), OperandWidth(0), X86_RAX);
			code_.NotReg(OperandWidth(0), X86_RAX);
			StoreResult(Operand(0), OperandWidth(0), X86_RAX);
			break;
		case CY86_FAM_BITWISE:
			EmitBinary(opcode.variant == 0 ? X86_AND
			           : opcode.variant == 1 ? X86_OR : X86_XOR);
			break;
		case CY86_FAM_SHIFT:
			EmitShift(opcode.variant == 0 ? X86_SHL
			          : opcode.variant == 1 ? X86_SAR : X86_SHR);
			break;
		case CY86_FAM_ADDSUB:
			EmitBinary(opcode.variant == 0 ? X86_ADD : X86_SUB);
			break;
		case CY86_FAM_MUL:
			EmitMul(opcode.variant);
			break;
		case CY86_FAM_DIV:
			EmitDiv(opcode.variant);
			break;
		case CY86_FAM_ICMP:
			EmitIntCompare(opcode.variant);
			break;
		case CY86_FAM_FCMP:
			EmitFloatCompare(opcode.variant);
			break;
		case CY86_FAM_FARITH:
			EmitFloatArith(opcode.variant);
			break;
		case CY86_FAM_TO_F80:
			EmitToF80(opcode.variant);
			break;
		case CY86_FAM_FROM_F80:
			EmitFromF80(opcode.variant);
			break;
		case CY86_FAM_SYSCALL:
			EmitSyscall(opcode.variant);
			break;
		case CY86_FAM_DATA:
			throw logic_error("data opcodes are not code");
		}
	}

	const CY86Statement& stmt_;
	X86CodeBuffer code_;
};

// data8..data64: the immediate's bytes, aligned to their size; label
// values resolve at layout through a data patch.
void TranslateData(const CY86Statement& stmt, ImageItem& item)
{
	size_t nbytes =
		static_cast<size_t>(stmt.opcode->operands[0].width) / 8;
	const CY86Immediate& imm = stmt.operands[0].imm;
	item.align = nbytes;
	if (imm.has_label)
	{
		item.bytes.assign(nbytes, 0);
		X86Patch patch;
		patch.offset = 0;
		patch.size = static_cast<int>(nbytes);
		patch.imm = X86Imm::Label(imm.label, imm.addend);
		item.patches.push_back(patch);
	}
	else
	{
		item.bytes.assign(imm.bytes.begin(), imm.bytes.end());
	}
}

}  // namespace

void TranslateCY86Program(const CY86Program& program, ProgramImage& image)
{
	image.SetLabelCount(static_cast<int>(program.label_names.size()));
	for (size_t i = 0; i < program.statements.size(); i++)
	{
		const CY86Statement& stmt = program.statements[i];
		ImageItem item;
		if (!stmt.opcode)
		{
			item.align = stmt.data_align;
			item.bytes.assign(stmt.data.begin(), stmt.data.end());
		}
		else if (stmt.opcode->family == CY86_FAM_DATA)
		{
			TranslateData(stmt, item);
		}
		else
		{
			StatementTranslator translator(stmt);
			translator.Translate(item);
		}
		image.AddItem(item);
	}
	for (size_t id = 0; id < program.label_names.size(); id++)
		image.BindLabel(static_cast<int>(id), program.label_statement[id]);
	if (program.start_label >= 0)
		image.SetEntryLabel(program.start_label);
}
