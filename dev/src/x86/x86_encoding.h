#pragma once

#include <cstddef>
#include <vector>

using std::size_t;
using std::vector;

// PA9 x86-64 instruction object model and machine-code encoder.
//
// An X86Instruction names a mnemonic form plus register/memory/
// immediate operands; EncodeX86Instruction turns one into the byte
// sequence (legacy prefixes, REX, opcode, ModRM, SIB, displacement,
// immediate). Immediates may depend on a label address that is only
// known after image layout, so every label-bearing form has a
// value-independent encoded length (mov r64, imm64, absolute [disp32]
// operands, rel32 control transfers, and fixed-width data) and
// reports the label field as an X86Patch to fill in later.
// X86CodeBuffer accumulates the encoded statement body and its
// pending patches. Later assignments extend the mnemonic table; the
// encoder core is form-driven, not opcode-driven.

// Hardware register ids; bit 3 is the REX extension bit.
enum EX86Reg
{
	X86_RAX = 0, X86_RCX = 1, X86_RDX = 2, X86_RBX = 3,
	X86_RSP = 4, X86_RBP = 5, X86_RSI = 6, X86_RDI = 7,
	X86_R8 = 8, X86_R9 = 9, X86_R10 = 10, X86_R11 = 11,
	X86_R12 = 12, X86_R13 = 13, X86_R14 = 14, X86_R15 = 15
};

// Condition codes: the cc nibble shared by SETcc and Jcc.
enum EX86Cond
{
	X86_CC_B = 2,    // below (CF=1): unsigned <, x87 <
	X86_CC_AE = 3,   // above or equal: unsigned >=
	X86_CC_E = 4,    // equal
	X86_CC_NE = 5,   // not equal
	X86_CC_BE = 6,   // below or equal: unsigned <=
	X86_CC_A = 7,    // above: unsigned >
	X86_CC_S = 8,    // sign set
	X86_CC_NS = 9,   // sign clear
	X86_CC_L = 12,   // less: signed <
	X86_CC_GE = 13,  // signed >=
	X86_CC_LE = 14,  // signed <=
	X86_CC_G = 15    // signed >
};

// A 64-bit immediate, possibly label-relative: the final value is
// label address + addend, resolved at image layout time.
struct X86Imm
{
	X86Imm() : has_label(false), label(0), addend(0) {}

	static X86Imm Constant(unsigned long long value)
	{
		X86Imm imm;
		imm.addend = value;
		return imm;
	}

	static X86Imm Label(int label, unsigned long long addend)
	{
		X86Imm imm;
		imm.has_label = true;
		imm.label = label;
		imm.addend = addend;
		return imm;
	}

	bool has_label;
	int label;
	unsigned long long addend;
};

// How a resolved patch value lands in its field. The CPU
// sign-extends disp32 and rel32 fields back to 64 bits, so those
// kinds fail layout instead of silently truncating a value the field
// cannot represent.
enum EX86PatchKind
{
	X86_PATCH_TRUNC,  // little-endian value truncated to size (data)
	X86_PATCH_ABS,    // absolute address; must survive sign-extension
	X86_PATCH_PCREL   // value minus the address after the field
};

// A pending little-endian fixup: write the resolved immediate, as
// interpreted by `kind`, at `offset` within the emitted bytes.
struct X86Patch
{
	X86Patch() : offset(0), size(0), kind(X86_PATCH_TRUNC) {}

	size_t offset;
	int size;
	EX86PatchKind kind;
	X86Imm imm;
};

// Memory operand: [base register + displacement], or an absolute
// [disp32] form (no base register) whose address may be a label
// resolved at layout.
struct X86Mem
{
	X86Mem() : has_base(true), base(X86_RAX), disp(0) {}
	X86Mem(int base, int disp) : has_base(true), base(base), disp(disp) {}

	static X86Mem Absolute(const X86Imm& addr)
	{
		X86Mem mem;
		mem.has_base = false;
		mem.abs = addr;
		return mem;
	}

	bool has_base;
	int base;
	int disp;
	X86Imm abs;
};

enum EX86Mnemonic
{
	X86_MOV_RI,    // mov r64, imm64 (label-bearing immediate form)
	X86_MOV_RI32,  // mov r32, imm32 (zero-extends; constant only)
	X86_MOV_RI32S, // mov r64, imm32 (sign-extends; constant only)
	X86_MOV_RR,    // mov r(w), r(w)
	X86_MOV_RM,    // mov r(w), [mem]
	X86_MOV_MR,    // mov [mem], r(w)
	X86_ADD,       // add r(w), r(w)
	X86_SUB,
	X86_AND,
	X86_OR,
	X86_XOR,
	X86_CMP,
	X86_TEST,
	X86_NOT,       // not r(w)
	X86_SHL,       // shift r(w) by cl
	X86_SHR,
	X86_SAR,
	X86_SHR_I,     // shr r(w), imm8 (count in imm.addend)
	X86_MUL,       // one-operand forms: (rdx:)rax op= r(w)
	X86_IMUL,
	X86_DIV,
	X86_IDIV,
	X86_MOVZX,     // movzx r32, r(w: 8/16); zero-extends to 64
	X86_MOVSX,     // movsx r64, r(w: 8/16/32)
	X86_SEXT_ACC,  // w=8: cbw; 16: cwd; 32: cdq; 64: cqo
	X86_SETCC,     // setcc r8
	X86_JCC_REL8,
	X86_JMP_REL8,
	X86_JCC_REL32, // jcc rel32 to a label target (imm)
	X86_JMP_REL32, // jmp rel32 to a label target (imm)
	X86_CALL_REL32,// call rel32 to a label target (imm)
	X86_JMP_R,     // jmp r64
	X86_CALL_R,    // call r64
	X86_RET,
	X86_SYSCALL,
	X86_FLD,       // fld m(w: 32/64/80)
	X86_FSTP,      // fstp m(w: 32/64/80)
	X86_FILD,      // fild m(w: 16/32/64)
	X86_FISTP,     // fistp m(w: 16/32/64)
	X86_FADD_M64,  // fadd m64fp
	X86_FSUB_M64,  // fsub m64fp
	X86_FADDP,     // st(1) op= st(0); pop
	X86_FSUBP,
	X86_FMULP,
	X86_FDIVP,
	X86_FCOMIP,    // compare st(0) with st(1); pop
	X86_FSTP_ST0,  // pop the x87 stack

	// PA28 machine-IR forms.
	X86_ADD_RI,    // ALU r/m(w), imm (imm8/16/32 by width; 64 sign-extends)
	X86_OR_RI,
	X86_AND_RI,
	X86_SUB_RI,
	X86_XOR_RI,
	X86_CMP_RI,
	X86_TEST_RI,   // test r/m(w), imm
	X86_CMP_MR,    // cmp r/m(w), r(w)
	X86_IMUL_RR,   // imul r(w), r/m(w)
	X86_IMUL_RRI,  // imul r64, r/m64, imm32
	X86_NEG,       // neg r/m(w)
	X86_LEA,       // lea r64, [mem]
	X86_BSWAP,     // bswap r(w: 32/64)
	X86_ROR_I,     // ror r/m(w), imm8 (count in imm.addend)
	X86_BTC_I,     // btc r/m64, imm8 (bit in imm.addend)
	X86_PUSH_R,    // push r64
	X86_POP_R,     // pop r64
	X86_XCHG_MR,   // xchg r/m(w), r(w) (implicitly locked with mem)
	X86_XADD_LOCK, // lock xadd r/m(w), r(w)
	X86_CMPXCHG_LOCK, // lock cmpxchg r/m(w), r(w)
	X86_MFENCE,
	X86_REP_MOVSB, // rep movsb (rcx bytes rsi -> rdi)
	X86_REP_STOSB, // rep stosb (rcx copies of al -> rdi)

	// SSE scalar forms; the reg field holds an xmm id except where a
	// GPR is named. Wide (REX.W) forms carry width 64.
	X86_MOVSS_RM,  // movss xmm, xmm/m32
	X86_MOVSS_MR,  // movss m32, xmm
	X86_MOVSD_RM,
	X86_MOVSD_MR,
	X86_ADDSS,
	X86_ADDSD,
	X86_SUBSS,
	X86_SUBSD,
	X86_MULSS,
	X86_MULSD,
	X86_DIVSS,
	X86_DIVSD,
	X86_UCOMISS,   // ucomiss xmm, xmm/m32
	X86_UCOMISD,
	X86_CVTSI2SS,  // cvtsi2ss xmm, r/m64
	X86_CVTSI2SD,
	X86_CVTTSS2SI, // cvttss2si r64, xmm/m32
	X86_CVTTSD2SI,
	X86_CVTSS2SD,
	X86_CVTSD2SS,
	X86_MOVQ_XR,   // movq xmm, r64
	X86_MOVQ_RX,   // movq r64, xmm
	X86_XORPS,     // xorps xmm, xmm/m128 (mem must be 16-aligned)

	X86_FCHS,      // negate st(0)
	X86_FNSTCW,    // fnstcw m16
	X86_FLDCW      // fldcw m16
};

struct X86Instruction
{
	X86Instruction()
		: mnemonic(X86_RET), width(0), cond(0), reg(0), rm_reg(0),
		  rm_is_mem(false), rel(0)
	{}

	EX86Mnemonic mnemonic;
	int width;       // operand width in bits
	int cond;        // EX86Cond for SETCC / JCC_REL8
	int reg;         // register operand (dst of loads, src of stores)
	int rm_reg;      // register r/m operand
	bool rm_is_mem;  // r/m is `mem`, not `rm_reg`
	X86Mem mem;
	X86Imm imm;      // MOV_RI value; SHR_I count
	int rel;         // JCC_REL8 / JMP_REL8 displacement
};

struct X86MachineCode
{
	X86MachineCode() : has_patch(false) {}

	vector<unsigned char> bytes;
	bool has_patch;
	X86Patch patch;
};

X86MachineCode EncodeX86Instruction(const X86Instruction& instr);

// Accumulates encoded instructions for one statement and the pending
// immediate patches (offsets relative to the buffer start).
class X86CodeBuffer
{
public:
	const vector<unsigned char>& bytes() const { return bytes_; }
	const vector<X86Patch>& patches() const { return patches_; }

	void Append(const X86Instruction& instr);
	void AppendBuffer(const X86CodeBuffer& other);

	// Convenience emitters for the forms the PA9 translator uses.
	void MovRegImm(int reg, const X86Imm& imm);
	void MovRegReg(int width, int dst, int src);
	void MovRegMem(int width, int reg, const X86Mem& mem);
	void MovMemReg(int width, const X86Mem& mem, int reg);
	void Alu(EX86Mnemonic op, int width, int dst, int src);
	void NotReg(int width, int reg);
	void ShiftRegCl(EX86Mnemonic op, int width, int reg);
	void ShrRegImm(int width, int reg, int count);
	void MulDiv(EX86Mnemonic op, int width, int reg);
	void Movzx(int src_width, int dst, int src);
	void Movsx64(int src_width, int dst, int src);
	void SignExtendAcc(int width);
	void Setcc(int cond, int reg);
	void JccRel8(int cond, int rel);
	void JmpRel8(int rel);
	void JccRel32(int cond, const X86Imm& target);
	void JmpRel32(const X86Imm& target);
	void CallRel32(const X86Imm& target);
	void JmpReg(int reg);
	void CallReg(int reg);
	void Ret();
	void Syscall();
	void X87Mem(EX86Mnemonic op, int width, const X86Mem& mem);
	void X87Stack(EX86Mnemonic op);

	// PA28 additions.
	void AluRegImm(EX86Mnemonic op, int width, int reg,
	               unsigned long long imm);
	void AluMemImm(EX86Mnemonic op, int width, const X86Mem& mem,
	               unsigned long long imm);
	void AluRegMem(EX86Mnemonic op, int width, int reg, const X86Mem& mem);
	void MovzxMem(int src_width, int reg, const X86Mem& mem);
	void Lea(int reg, const X86Mem& mem);
	void ImulRegReg(int dst, int src);
	void ImulRegImm(int reg, unsigned long long imm);
	void NegReg(int width, int reg);
	void Bswap(int width, int reg);
	void RorRegImm(int width, int reg, int count);
	void BtcRegImm(int reg, int bit);
	void Push(int reg);
	void Pop(int reg);
	void Fixed(EX86Mnemonic op);
	void SseRegReg(EX86Mnemonic op, int reg, int rm_reg);
	void SseRegMem(EX86Mnemonic op, int reg, const X86Mem& mem);

private:
	vector<unsigned char> bytes_;
	vector<X86Patch> patches_;
};
