#include "x86/x86_encoding.h"

#include <stdexcept>

using std::logic_error;

namespace {

// True when accessing this id as a byte register requires a REX
// prefix to mean spl/bpl/sil/dil instead of ah/ch/dh/bh.
bool NeedsLowByteRex(int reg)
{
	return reg >= X86_RSP && reg <= X86_RDI;
}

void PutLittleEndian(vector<unsigned char>& out, unsigned long long value,
                     int size)
{
	for (int i = 0; i < size; i++)
		out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}

// One instruction built around a ModRM byte. `opcode8` is the 8-bit
// form (-1 if the form has none); `opcode` covers 16/32/64 widths.
// Two-byte 0F-page opcodes are passed as 0x0Fxx. `width` 0 means no
// operand-size handling (x87 and default-64 forms). `reg_field` is a
// register id, or an opcode extension when `reg_is_ext`.
struct ModRMForm
{
	ModRMForm()
		: opcode8(-1), opcode(0), reg_field(0), reg_is_ext(false),
		  width(0), rm_op_width(0), prefix(0)
	{}

	int opcode8;
	int opcode;
	int reg_field;
	bool reg_is_ext;
	int width;
	int rm_op_width;  // width of the r/m operand for byte-REX checks
	int prefix;       // lock or SSE mandatory prefix, before 66/REX
};

void EmitModRM(const ModRMForm& form, const X86Instruction& instr,
               X86MachineCode& code)
{
	vector<unsigned char>& out = code.bytes;
	int opcode = (form.width == 8 && form.opcode8 >= 0) ? form.opcode8
	                                                    : form.opcode;
	if (form.prefix)
		out.push_back(static_cast<unsigned char>(form.prefix));
	if (form.width == 16)
		out.push_back(0x66);

	bool rex_w = form.width == 64;
	bool rex_r = !form.reg_is_ext && (form.reg_field & 8) != 0;
	int rm_id = instr.rm_is_mem
		? (instr.mem.has_base ? instr.mem.base : 0)
		: instr.rm_reg;
	bool rex_b = (rm_id & 8) != 0;
	bool rex_low =
		(form.width == 8 && !form.reg_is_ext &&
		 NeedsLowByteRex(form.reg_field)) ||
		(form.rm_op_width == 8 && !instr.rm_is_mem &&
		 NeedsLowByteRex(instr.rm_reg));
	if (rex_w || rex_r || rex_b || rex_low)
		out.push_back(0x40 | (rex_w << 3) | (rex_r << 2) | (rex_b ? 1 : 0));

	if (opcode & 0xFF00)
		out.push_back(static_cast<unsigned char>(opcode >> 8));
	out.push_back(static_cast<unsigned char>(opcode & 0xFF));

	int reg_bits = (form.reg_field & 7) << 3;
	if (!instr.rm_is_mem)
	{
		out.push_back(0xC0 | reg_bits | (instr.rm_reg & 7));
		return;
	}

	if (!instr.mem.has_base)
	{
		// mod=00 with SIB base=101 and no index: absolute [disp32].
		out.push_back(reg_bits | 4);
		out.push_back(0x25);
		if (instr.mem.abs.has_label)
		{
			code.has_patch = true;
			code.patch.offset = out.size();
			code.patch.size = 4;
			code.patch.kind = X86_PATCH_ABS;
			code.patch.imm = instr.mem.abs;
		}
		PutLittleEndian(out, instr.mem.abs.addend, 4);
		return;
	}

	int base = instr.mem.base & 7;
	int disp = instr.mem.disp;
	int mod;
	if (disp == 0 && base != 5)
		mod = 0;  // rbp/r13 have no disp-less encoding
	else if (disp >= -128 && disp <= 127)
		mod = 1;
	else
		mod = 2;
	out.push_back((mod << 6) | reg_bits | base);
	if (base == 4)
		out.push_back(0x24);  // SIB: no index, base rsp/r12
	if (mod == 1)
		out.push_back(static_cast<unsigned char>(disp));
	else if (mod == 2)
		PutLittleEndian(out, static_cast<unsigned long long>(
		                         static_cast<unsigned>(disp)), 4);
}

// ALU "r <- r/m" opcodes (the byte form is opcode-1's even pair).
bool AluRegRmOpcodes(EX86Mnemonic m, int& opcode8, int& opcode)
{
	switch (m)
	{
	case X86_ADD: opcode8 = 0x02; opcode = 0x03; return true;
	case X86_OR:  opcode8 = 0x0A; opcode = 0x0B; return true;
	case X86_AND: opcode8 = 0x22; opcode = 0x23; return true;
	case X86_SUB: opcode8 = 0x2A; opcode = 0x2B; return true;
	case X86_XOR: opcode8 = 0x32; opcode = 0x33; return true;
	case X86_CMP: opcode8 = 0x3A; opcode = 0x3B; return true;
	default: return false;
	}
}

bool UnaryExtension(EX86Mnemonic m, int& opcode8, int& opcode, int& ext)
{
	switch (m)
	{
	case X86_NOT:  opcode8 = 0xF6; opcode = 0xF7; ext = 2; return true;
	case X86_NEG:  opcode8 = 0xF6; opcode = 0xF7; ext = 3; return true;
	case X86_MUL:  opcode8 = 0xF6; opcode = 0xF7; ext = 4; return true;
	case X86_IMUL: opcode8 = 0xF6; opcode = 0xF7; ext = 5; return true;
	case X86_DIV:  opcode8 = 0xF6; opcode = 0xF7; ext = 6; return true;
	case X86_IDIV: opcode8 = 0xF6; opcode = 0xF7; ext = 7; return true;
	case X86_SHL:  opcode8 = 0xD2; opcode = 0xD3; ext = 4; return true;
	case X86_SHR:  opcode8 = 0xD2; opcode = 0xD3; ext = 5; return true;
	case X86_SAR:  opcode8 = 0xD2; opcode = 0xD3; ext = 7; return true;
	default: return false;
	}
}

// x87 memory forms: opcode byte and ModRM extension per operand width.
bool X87MemForm(EX86Mnemonic m, int width, int& opcode, int& ext)
{
	switch (m)
	{
	case X86_FLD:
		opcode = width == 32 ? 0xD9 : width == 64 ? 0xDD : 0xDB;
		ext = width == 80 ? 5 : 0;
		return width == 32 || width == 64 || width == 80;
	case X86_FSTP:
		opcode = width == 32 ? 0xD9 : width == 64 ? 0xDD : 0xDB;
		ext = width == 80 ? 7 : 3;
		return width == 32 || width == 64 || width == 80;
	case X86_FILD:
		opcode = width == 32 ? 0xDB : 0xDF;
		ext = width == 64 ? 5 : 0;
		return width == 16 || width == 32 || width == 64;
	case X86_FISTP:
		opcode = width == 32 ? 0xDB : 0xDF;
		ext = width == 64 ? 7 : 3;
		return width == 16 || width == 32 || width == 64;
	case X86_FADD_M64:
		opcode = 0xDC;
		ext = 0;
		return width == 64;
	case X86_FSUB_M64:
		opcode = 0xDC;
		ext = 4;
		return width == 64;
	case X86_FNSTCW:
		opcode = 0xD9;
		ext = 7;
		return true;
	case X86_FLDCW:
		opcode = 0xD9;
		ext = 5;
		return true;
	default:
		return false;
	}
}

// Two-opcode (8-bit / wider) ModRM forms added for the machine-IR
// backend; the lock forms carry an F0 prefix.
bool PairedModRMForm(EX86Mnemonic m, ModRMForm& form)
{
	switch (m)
	{
	case X86_CMP_MR: form.opcode8 = 0x38; form.opcode = 0x39; return true;
	case X86_XCHG_MR: form.opcode8 = 0x86; form.opcode = 0x87; return true;
	case X86_XADD_LOCK:
		form.prefix = 0xF0;
		form.opcode8 = 0x0FC0;
		form.opcode = 0x0FC1;
		return true;
	case X86_CMPXCHG_LOCK:
		form.prefix = 0xF0;
		form.opcode8 = 0x0FB0;
		form.opcode = 0x0FB1;
		return true;
	case X86_IMUL_RR: form.opcode = 0x0FAF; return true;
	case X86_LEA: form.opcode = 0x8D; return true;
	default: return false;
	}
}

// SSE scalar forms: mandatory prefix plus 0F-page opcode. Width 64
// marks the forms whose GPR operand takes REX.W; xmm ids never need
// the byte-register REX check.
bool SseForm(EX86Mnemonic m, ModRMForm& form)
{
	switch (m)
	{
	case X86_MOVSS_RM: form.prefix = 0xF3; form.opcode = 0x0F10; break;
	case X86_MOVSS_MR: form.prefix = 0xF3; form.opcode = 0x0F11; break;
	case X86_MOVSD_RM: form.prefix = 0xF2; form.opcode = 0x0F10; break;
	case X86_MOVSD_MR: form.prefix = 0xF2; form.opcode = 0x0F11; break;
	case X86_ADDSS: form.prefix = 0xF3; form.opcode = 0x0F58; break;
	case X86_ADDSD: form.prefix = 0xF2; form.opcode = 0x0F58; break;
	case X86_SUBSS: form.prefix = 0xF3; form.opcode = 0x0F5C; break;
	case X86_SUBSD: form.prefix = 0xF2; form.opcode = 0x0F5C; break;
	case X86_MULSS: form.prefix = 0xF3; form.opcode = 0x0F59; break;
	case X86_MULSD: form.prefix = 0xF2; form.opcode = 0x0F59; break;
	case X86_DIVSS: form.prefix = 0xF3; form.opcode = 0x0F5E; break;
	case X86_DIVSD: form.prefix = 0xF2; form.opcode = 0x0F5E; break;
	case X86_UCOMISS: form.opcode = 0x0F2E; break;
	case X86_UCOMISD: form.prefix = 0x66; form.opcode = 0x0F2E; break;
	case X86_CVTSI2SS:
		form.prefix = 0xF3; form.opcode = 0x0F2A; form.width = 64; break;
	case X86_CVTSI2SD:
		form.prefix = 0xF2; form.opcode = 0x0F2A; form.width = 64; break;
	case X86_CVTTSS2SI:
		form.prefix = 0xF3; form.opcode = 0x0F2C; form.width = 64; break;
	case X86_CVTTSD2SI:
		form.prefix = 0xF2; form.opcode = 0x0F2C; form.width = 64; break;
	case X86_CVTSS2SD: form.prefix = 0xF3; form.opcode = 0x0F5A; break;
	case X86_CVTSD2SS: form.prefix = 0xF2; form.opcode = 0x0F5A; break;
	case X86_MOVQ_XR:
		form.prefix = 0x66; form.opcode = 0x0F6E; form.width = 64; break;
	case X86_MOVQ_RX:
		form.prefix = 0x66; form.opcode = 0x0F7E; form.width = 64; break;
	case X86_XORPS: form.opcode = 0x0F57; break;
	default: return false;
	}
	form.rm_op_width = 0;
	return true;
}

// r/m-with-extension opcodes carrying a trailing immediate.
bool RmImmForm(EX86Mnemonic m, int width, ModRMForm& form, int& imm_size)
{
	imm_size = width == 8 ? 1 : width == 16 ? 2 : 4;
	form.opcode8 = 0x80;
	form.opcode = 0x81;
	form.reg_is_ext = true;
	switch (m)
	{
	case X86_ADD_RI: form.reg_field = 0; return true;
	case X86_OR_RI: form.reg_field = 1; return true;
	case X86_AND_RI: form.reg_field = 4; return true;
	case X86_SUB_RI: form.reg_field = 5; return true;
	case X86_XOR_RI: form.reg_field = 6; return true;
	case X86_CMP_RI: form.reg_field = 7; return true;
	case X86_TEST_RI:
		form.opcode8 = 0xF6;
		form.opcode = 0xF7;
		form.reg_field = 0;
		return true;
	case X86_ROR_I:
		form.opcode8 = 0xC0;
		form.opcode = 0xC1;
		form.reg_field = 1;
		imm_size = 1;
		return true;
	case X86_BTC_I:
		form.opcode8 = -1;
		form.opcode = 0x0FBA;
		form.reg_field = 7;
		imm_size = 1;
		return true;
	default: return false;
	}
}

// Fixed byte sequences with no operands to encode.
bool FixedBytes(const X86Instruction& instr, vector<unsigned char>& out)
{
	switch (instr.mnemonic)
	{
	case X86_RET: out.push_back(0xC3); return true;
	case X86_SYSCALL: out.push_back(0x0F); out.push_back(0x05); return true;
	case X86_FADDP: out.push_back(0xDE); out.push_back(0xC1); return true;
	case X86_FSUBP: out.push_back(0xDE); out.push_back(0xE9); return true;
	case X86_FMULP: out.push_back(0xDE); out.push_back(0xC9); return true;
	case X86_FDIVP: out.push_back(0xDE); out.push_back(0xF9); return true;
	case X86_FCOMIP: out.push_back(0xDF); out.push_back(0xF1); return true;
	case X86_FSTP_ST0: out.push_back(0xDD); out.push_back(0xD8); return true;
	case X86_FCHS: out.push_back(0xD9); out.push_back(0xE0); return true;
	case X86_MFENCE:
		out.push_back(0x0F);
		out.push_back(0xAE);
		out.push_back(0xF0);
		return true;
	case X86_REP_MOVSB: out.push_back(0xF3); out.push_back(0xA4); return true;
	case X86_REP_STOSB: out.push_back(0xF3); out.push_back(0xAA); return true;
	case X86_SEXT_ACC:
		// cbw / cwd / cdq / cqo for the 8/16/32/64-bit dividend.
		if (instr.width == 8)
		{
			out.push_back(0x66);
			out.push_back(0x98);
		}
		else
		{
			if (instr.width == 16)
				out.push_back(0x66);
			else if (instr.width == 64)
				out.push_back(0x48);
			out.push_back(0x99);
		}
		return true;
	default:
		return false;
	}
}

// bswap/push/pop encode the register in the opcode byte itself.
bool OpcodeRegForm(const X86Instruction& instr, vector<unsigned char>& out)
{
	int reg = instr.reg;
	switch (instr.mnemonic)
	{
	case X86_BSWAP:
	{
		int rex = (instr.width == 64 ? 8 : 0) | ((reg & 8) ? 1 : 0);
		if (rex)
			out.push_back(static_cast<unsigned char>(0x40 | rex));
		out.push_back(0x0F);
		out.push_back(static_cast<unsigned char>(0xC8 | (reg & 7)));
		return true;
	}
	case X86_PUSH_R:
	case X86_POP_R:
		if (reg & 8)
			out.push_back(0x41);
		out.push_back(static_cast<unsigned char>(
			(instr.mnemonic == X86_PUSH_R ? 0x50 : 0x58) | (reg & 7)));
		return true;
	default:
		return false;
	}
}

X86MachineCode EncodeModRMInstruction(const X86Instruction& instr)
{
	X86MachineCode code;
	ModRMForm form;
	form.width = instr.width;
	form.rm_op_width = instr.width;
	form.reg_field = instr.reg;
	int ext = 0;

	switch (instr.mnemonic)
	{
	case X86_MOV_RR:
	case X86_MOV_RM:
		form.opcode8 = 0x8A;
		form.opcode = 0x8B;
		break;
	case X86_MOV_MR:
		form.opcode8 = 0x88;
		form.opcode = 0x89;
		break;
	case X86_TEST:
		form.opcode8 = 0x84;
		form.opcode = 0x85;
		break;
	case X86_SETCC:
		form.opcode8 = 0x0F90 | instr.cond;
		form.opcode = form.opcode8;
		form.reg_field = 0;
		form.reg_is_ext = true;
		break;
	case X86_MOVZX:
		form.opcode = instr.width == 8 ? 0x0FB6 : 0x0FB7;
		form.rm_op_width = instr.width;
		form.width = 32;
		break;
	case X86_MOVSX:
		form.opcode = instr.width == 8 ? 0x0FBE
		            : instr.width == 16 ? 0x0FBF : 0x63;
		form.rm_op_width = instr.width;
		form.width = 64;
		break;
	case X86_JMP_R:
	case X86_CALL_R:
		form.opcode = 0xFF;
		form.reg_field = instr.mnemonic == X86_JMP_R ? 4 : 2;
		form.reg_is_ext = true;
		form.width = 0;  // 64-bit is the default operand size
		break;
	default:
		if (AluRegRmOpcodes(instr.mnemonic, form.opcode8, form.opcode))
			break;
		if (PairedModRMForm(instr.mnemonic, form))
			break;
		if (SseForm(instr.mnemonic, form))
			break;
		if (UnaryExtension(instr.mnemonic, form.opcode8, form.opcode, ext))
		{
			form.reg_field = ext;
			form.reg_is_ext = true;
			break;
		}
		if (X87MemForm(instr.mnemonic, instr.width, form.opcode, ext))
		{
			form.reg_field = ext;
			form.reg_is_ext = true;
			form.width = 0;  // x87 widths pick opcodes, not prefixes
			form.rm_op_width = 0;
			break;
		}
		throw logic_error("unsupported x86 instruction form");
	}

	EmitModRM(form, instr, code);
	return code;
}

}  // namespace

X86MachineCode EncodeX86Instruction(const X86Instruction& instr)
{
	X86MachineCode code;
	switch (instr.mnemonic)
	{
	case X86_MOV_RI:
		code.bytes.push_back(0x48 | ((instr.reg & 8) ? 1 : 0));
		code.bytes.push_back(0xB8 | (instr.reg & 7));
		code.has_patch = true;
		code.patch.offset = code.bytes.size();
		code.patch.size = 8;
		code.patch.imm = instr.imm;
		PutLittleEndian(code.bytes, instr.imm.addend, 8);
		return code;
	case X86_MOV_RI32:
		if (instr.reg & 8)
			code.bytes.push_back(0x41);
		code.bytes.push_back(0xB8 | (instr.reg & 7));
		PutLittleEndian(code.bytes, instr.imm.addend, 4);
		return code;
	case X86_MOV_RI32S:
		code.bytes.push_back(0x48 | ((instr.reg & 8) ? 1 : 0));
		code.bytes.push_back(0xC7);
		code.bytes.push_back(0xC0 | (instr.reg & 7));
		PutLittleEndian(code.bytes, instr.imm.addend, 4);
		return code;
	case X86_JMP_REL32:
	case X86_CALL_REL32:
	case X86_JCC_REL32:
		if (instr.mnemonic == X86_JCC_REL32)
		{
			code.bytes.push_back(0x0F);
			code.bytes.push_back(0x80 | instr.cond);
		}
		else
		{
			code.bytes.push_back(
				instr.mnemonic == X86_JMP_REL32 ? 0xE9 : 0xE8);
		}
		code.has_patch = true;
		code.patch.offset = code.bytes.size();
		code.patch.size = 4;
		code.patch.kind = X86_PATCH_PCREL;
		code.patch.imm = instr.imm;
		PutLittleEndian(code.bytes, 0, 4);
		return code;
	case X86_SHR_I:
	{
		X86Instruction shifted = instr;
		shifted.rm_reg = instr.reg;
		shifted.reg = 5;  // /5
		ModRMForm form;
		form.opcode8 = 0xC0;
		form.opcode = 0xC1;
		form.reg_field = 5;
		form.reg_is_ext = true;
		form.width = instr.width;
		form.rm_op_width = instr.width;
		EmitModRM(form, shifted, code);
		code.bytes.push_back(
			static_cast<unsigned char>(instr.imm.addend & 0xFF));
		return code;
	}
	case X86_JCC_REL8:
		code.bytes.push_back(0x70 | instr.cond);
		code.bytes.push_back(static_cast<unsigned char>(instr.rel));
		return code;
	case X86_JMP_REL8:
		code.bytes.push_back(0xEB);
		code.bytes.push_back(static_cast<unsigned char>(instr.rel));
		return code;
	default:
	{
		if (OpcodeRegForm(instr, code.bytes))
			return code;
		ModRMForm form;
		int imm_size = 0;
		form.width = instr.width;
		form.rm_op_width = instr.width;
		if (RmImmForm(instr.mnemonic, instr.width, form, imm_size))
		{
			EmitModRM(form, instr, code);
			PutLittleEndian(code.bytes, instr.imm.addend, imm_size);
			return code;
		}
		if (instr.mnemonic == X86_IMUL_RRI)
		{
			form = ModRMForm();  // a failed RmImmForm probe writes it
			form.width = instr.width;
			form.rm_op_width = instr.width;
			form.opcode = 0x69;
			form.reg_field = instr.reg;
			EmitModRM(form, instr, code);
			PutLittleEndian(code.bytes, instr.imm.addend, 4);
			return code;
		}
		if (FixedBytes(instr, code.bytes))
			return code;
		return EncodeModRMInstruction(instr);
	}
	}
}

void X86CodeBuffer::Append(const X86Instruction& instr)
{
	X86MachineCode code = EncodeX86Instruction(instr);
	if (code.has_patch)
	{
		code.patch.offset += bytes_.size();
		patches_.push_back(code.patch);
	}
	bytes_.insert(bytes_.end(), code.bytes.begin(), code.bytes.end());
}

void X86CodeBuffer::AppendBuffer(const X86CodeBuffer& other)
{
	for (size_t i = 0; i < other.patches_.size(); i++)
	{
		X86Patch patch = other.patches_[i];
		patch.offset += bytes_.size();
		patches_.push_back(patch);
	}
	bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end());
}

void X86CodeBuffer::MovRegImm(int reg, const X86Imm& imm)
{
	X86Instruction instr;
	instr.width = 64;
	instr.reg = reg;
	instr.imm = imm;
	// A constant's value is known now: use the shortest encoding that
	// reproduces all 64 bits. Label values keep the full-width form.
	long long signed_value = static_cast<long long>(imm.addend);
	if (imm.has_label)
		instr.mnemonic = X86_MOV_RI;
	else if (imm.addend <= 0xFFFFFFFFull)
		instr.mnemonic = X86_MOV_RI32;
	else if (signed_value < 0 && signed_value >= -2147483648LL)
		instr.mnemonic = X86_MOV_RI32S;
	else
		instr.mnemonic = X86_MOV_RI;
	Append(instr);
}

void X86CodeBuffer::MovRegReg(int width, int dst, int src)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOV_RR;
	instr.width = width;
	instr.reg = dst;
	instr.rm_reg = src;
	Append(instr);
}

void X86CodeBuffer::MovRegMem(int width, int reg, const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOV_RM;
	instr.width = width;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::MovMemReg(int width, const X86Mem& mem, int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOV_MR;
	instr.width = width;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::Alu(EX86Mnemonic op, int width, int dst, int src)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.reg = dst;
	instr.rm_reg = src;
	Append(instr);
}

void X86CodeBuffer::NotReg(int width, int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_NOT;
	instr.width = width;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::ShiftRegCl(EX86Mnemonic op, int width, int reg)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::ShrRegImm(int width, int reg, int count)
{
	X86Instruction instr;
	instr.mnemonic = X86_SHR_I;
	instr.width = width;
	instr.reg = reg;
	instr.imm = X86Imm::Constant(static_cast<unsigned long long>(count));
	Append(instr);
}

void X86CodeBuffer::MulDiv(EX86Mnemonic op, int width, int reg)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::Movzx(int src_width, int dst, int src)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOVZX;
	instr.width = src_width;
	instr.reg = dst;
	instr.rm_reg = src;
	Append(instr);
}

void X86CodeBuffer::Movsx64(int src_width, int dst, int src)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOVSX;
	instr.width = src_width;
	instr.reg = dst;
	instr.rm_reg = src;
	Append(instr);
}

void X86CodeBuffer::SignExtendAcc(int width)
{
	X86Instruction instr;
	instr.mnemonic = X86_SEXT_ACC;
	instr.width = width;
	Append(instr);
}

void X86CodeBuffer::Setcc(int cond, int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_SETCC;
	instr.width = 8;
	instr.cond = cond;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::JccRel8(int cond, int rel)
{
	X86Instruction instr;
	instr.mnemonic = X86_JCC_REL8;
	instr.cond = cond;
	instr.rel = rel;
	Append(instr);
}

void X86CodeBuffer::JmpRel8(int rel)
{
	X86Instruction instr;
	instr.mnemonic = X86_JMP_REL8;
	instr.rel = rel;
	Append(instr);
}

void X86CodeBuffer::JccRel32(int cond, const X86Imm& target)
{
	X86Instruction instr;
	instr.mnemonic = X86_JCC_REL32;
	instr.cond = cond;
	instr.imm = target;
	Append(instr);
}

void X86CodeBuffer::JmpRel32(const X86Imm& target)
{
	X86Instruction instr;
	instr.mnemonic = X86_JMP_REL32;
	instr.imm = target;
	Append(instr);
}

void X86CodeBuffer::CallRel32(const X86Imm& target)
{
	X86Instruction instr;
	instr.mnemonic = X86_CALL_REL32;
	instr.imm = target;
	Append(instr);
}

void X86CodeBuffer::JmpReg(int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_JMP_R;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::CallReg(int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_CALL_R;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::Ret()
{
	X86Instruction instr;
	instr.mnemonic = X86_RET;
	Append(instr);
}

void X86CodeBuffer::Syscall()
{
	X86Instruction instr;
	instr.mnemonic = X86_SYSCALL;
	Append(instr);
}

void X86CodeBuffer::X87Mem(EX86Mnemonic op, int width, const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::X87Stack(EX86Mnemonic op)
{
	X86Instruction instr;
	instr.mnemonic = op;
	Append(instr);
}

void X86CodeBuffer::AluRegImm(EX86Mnemonic op, int width, int reg,
                              unsigned long long imm)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.rm_reg = reg;
	instr.imm = X86Imm::Constant(imm);
	Append(instr);
}

void X86CodeBuffer::AluMemImm(EX86Mnemonic op, int width, const X86Mem& mem,
                              unsigned long long imm)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.rm_is_mem = true;
	instr.mem = mem;
	instr.imm = X86Imm::Constant(imm);
	Append(instr);
}

// Covers both operand orders: the mnemonic fixes the direction (cmp
// vs cmp_mr, xchg/xadd/cmpxchg are all "r/m, r").
void X86CodeBuffer::AluRegMem(EX86Mnemonic op, int width, int reg,
                              const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = width;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::MovzxMem(int src_width, int reg, const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = X86_MOVZX;
	instr.width = src_width;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::Lea(int reg, const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = X86_LEA;
	instr.width = 64;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}

void X86CodeBuffer::ImulRegReg(int dst, int src)
{
	X86Instruction instr;
	instr.mnemonic = X86_IMUL_RR;
	instr.width = 64;
	instr.reg = dst;
	instr.rm_reg = src;
	Append(instr);
}

void X86CodeBuffer::ImulRegImm(int reg, unsigned long long imm)
{
	X86Instruction instr;
	instr.mnemonic = X86_IMUL_RRI;
	instr.width = 64;
	instr.reg = reg;
	instr.rm_reg = reg;
	instr.imm = X86Imm::Constant(imm);
	Append(instr);
}

void X86CodeBuffer::NegReg(int width, int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_NEG;
	instr.width = width;
	instr.rm_reg = reg;
	Append(instr);
}

void X86CodeBuffer::Bswap(int width, int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_BSWAP;
	instr.width = width;
	instr.reg = reg;
	Append(instr);
}

void X86CodeBuffer::RorRegImm(int width, int reg, int count)
{
	X86Instruction instr;
	instr.mnemonic = X86_ROR_I;
	instr.width = width;
	instr.rm_reg = reg;
	instr.imm = X86Imm::Constant(static_cast<unsigned long long>(count));
	Append(instr);
}

void X86CodeBuffer::BtcRegImm(int reg, int bit)
{
	X86Instruction instr;
	instr.mnemonic = X86_BTC_I;
	instr.width = 64;
	instr.rm_reg = reg;
	instr.imm = X86Imm::Constant(static_cast<unsigned long long>(bit));
	Append(instr);
}

void X86CodeBuffer::Push(int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_PUSH_R;
	instr.reg = reg;
	Append(instr);
}

void X86CodeBuffer::Pop(int reg)
{
	X86Instruction instr;
	instr.mnemonic = X86_POP_R;
	instr.reg = reg;
	Append(instr);
}

void X86CodeBuffer::Fixed(EX86Mnemonic op)
{
	X86Instruction instr;
	instr.mnemonic = op;
	Append(instr);
}

void X86CodeBuffer::SseRegReg(EX86Mnemonic op, int reg, int rm_reg)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = 0;
	instr.reg = reg;
	instr.rm_reg = rm_reg;
	Append(instr);
}

void X86CodeBuffer::SseRegMem(EX86Mnemonic op, int reg, const X86Mem& mem)
{
	X86Instruction instr;
	instr.mnemonic = op;
	instr.width = 0;
	instr.reg = reg;
	instr.rm_is_mem = true;
	instr.mem = mem;
	Append(instr);
}
