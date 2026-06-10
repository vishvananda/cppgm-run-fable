#include "cy86/cy86_parser.h"

#include <stdexcept>

using std::runtime_error;

namespace {

// ---------------------------------------------------------------
// Opcode table, transcribed from cy86-opcode.desc.
// ---------------------------------------------------------------

struct RawOpcode
{
	const char* name;
	ECY86Family family;
	int variant;
	const char* spec;
};

const RawOpcode kRawOpcodes[] =
{
	{"data8", CY86_FAM_DATA, 0, "rI8"},
	{"data16", CY86_FAM_DATA, 0, "rI16"},
	{"data32", CY86_FAM_DATA, 0, "rI32"},
	{"data64", CY86_FAM_DATA, 0, "rI64"},
	{"move8", CY86_FAM_MOVE, 0, "w8 r8"},
	{"move16", CY86_FAM_MOVE, 0, "w16 r16"},
	{"move32", CY86_FAM_MOVE, 0, "w32 r32"},
	{"move64", CY86_FAM_MOVE, 0, "w64 r64"},
	{"move80", CY86_FAM_MOVE, 0, "w80 r80"},
	{"jump", CY86_FAM_JUMP, 0, "ar64"},
	{"jumpif", CY86_FAM_JUMPIF, 0, "br8 ar64"},
	{"call", CY86_FAM_CALL, 0, "ar64"},
	{"ret", CY86_FAM_RET, 0, ""},
	{"not8", CY86_FAM_NOT, 0, "w8 r8"},
	{"not16", CY86_FAM_NOT, 0, "w16 r16"},
	{"not32", CY86_FAM_NOT, 0, "w32 r32"},
	{"not64", CY86_FAM_NOT, 0, "w64 r64"},
	{"and8", CY86_FAM_BITWISE, 0, "w8 r8 r8"},
	{"and16", CY86_FAM_BITWISE, 0, "w16 r16 r16"},
	{"and32", CY86_FAM_BITWISE, 0, "w32 r32 r32"},
	{"and64", CY86_FAM_BITWISE, 0, "w64 r64 r64"},
	{"or8", CY86_FAM_BITWISE, 1, "w8 r8 r8"},
	{"or16", CY86_FAM_BITWISE, 1, "w16 r16 r16"},
	{"or32", CY86_FAM_BITWISE, 1, "w32 r32 r32"},
	{"or64", CY86_FAM_BITWISE, 1, "w64 r64 r64"},
	{"xor8", CY86_FAM_BITWISE, 2, "w8 r8 r8"},
	{"xor16", CY86_FAM_BITWISE, 2, "w16 r16 r16"},
	{"xor32", CY86_FAM_BITWISE, 2, "w32 r32 r32"},
	{"xor64", CY86_FAM_BITWISE, 2, "w64 r64 r64"},
	{"lshift8", CY86_FAM_SHIFT, 0, "iw8 ir8 ur8"},
	{"lshift16", CY86_FAM_SHIFT, 0, "iw16 ir16 ur8"},
	{"lshift32", CY86_FAM_SHIFT, 0, "iw32 ir32 ur8"},
	{"lshift64", CY86_FAM_SHIFT, 0, "iw64 ir64 ur8"},
	{"srshift8", CY86_FAM_SHIFT, 1, "sw8 sr8 ur8"},
	{"srshift16", CY86_FAM_SHIFT, 1, "sw16 sr16 ur8"},
	{"srshift32", CY86_FAM_SHIFT, 1, "sw32 sr32 ur8"},
	{"srshift64", CY86_FAM_SHIFT, 1, "sw64 sr64 ur8"},
	{"urshift8", CY86_FAM_SHIFT, 2, "uw8 ur8 ur8"},
	{"urshift16", CY86_FAM_SHIFT, 2, "uw16 ur16 ur8"},
	{"urshift32", CY86_FAM_SHIFT, 2, "uw32 ur32 ur8"},
	{"urshift64", CY86_FAM_SHIFT, 2, "uw64 ur64 ur8"},
	{"s8convf80", CY86_FAM_TO_F80, 0, "fw80 sr8"},
	{"s16convf80", CY86_FAM_TO_F80, 0, "fw80 sr16"},
	{"s32convf80", CY86_FAM_TO_F80, 0, "fw80 sr32"},
	{"s64convf80", CY86_FAM_TO_F80, 0, "fw80 sr64"},
	{"u8convf80", CY86_FAM_TO_F80, 1, "fw80 ur8"},
	{"u16convf80", CY86_FAM_TO_F80, 1, "fw80 ur16"},
	{"u32convf80", CY86_FAM_TO_F80, 1, "fw80 ur32"},
	{"u64convf80", CY86_FAM_TO_F80, 1, "fw80 ur64"},
	{"f32convf80", CY86_FAM_TO_F80, 2, "fw80 fr32"},
	{"f64convf80", CY86_FAM_TO_F80, 2, "fw80 fr64"},
	{"f80convs8", CY86_FAM_FROM_F80, 0, "sw8 fr80"},
	{"f80convs16", CY86_FAM_FROM_F80, 0, "sw16 fr80"},
	{"f80convs32", CY86_FAM_FROM_F80, 0, "sw32 fr80"},
	{"f80convs64", CY86_FAM_FROM_F80, 0, "sw64 fr80"},
	{"f80convu8", CY86_FAM_FROM_F80, 1, "uw8 fr80"},
	{"f80convu16", CY86_FAM_FROM_F80, 1, "uw16 fr80"},
	{"f80convu32", CY86_FAM_FROM_F80, 1, "uw32 fr80"},
	{"f80convu64", CY86_FAM_FROM_F80, 1, "uw64 fr80"},
	{"f80convf32", CY86_FAM_FROM_F80, 2, "fw32 fr80"},
	{"f80convf64", CY86_FAM_FROM_F80, 2, "fw64 fr80"},
	{"iadd8", CY86_FAM_ADDSUB, 0, "iw8 ir8 ir8"},
	{"iadd16", CY86_FAM_ADDSUB, 0, "iw16 ir16 ir16"},
	{"iadd32", CY86_FAM_ADDSUB, 0, "iw32 ir32 ir32"},
	{"iadd64", CY86_FAM_ADDSUB, 0, "iw64 ir64 ir64"},
	{"fadd32", CY86_FAM_FARITH, 0, "fw32 fr32 fr32"},
	{"fadd64", CY86_FAM_FARITH, 0, "fw64 fr64 fr64"},
	{"fadd80", CY86_FAM_FARITH, 0, "fw80 fr80 fr80"},
	{"isub8", CY86_FAM_ADDSUB, 1, "iw8 ir8 ir8"},
	{"isub16", CY86_FAM_ADDSUB, 1, "iw16 ir16 ir16"},
	{"isub32", CY86_FAM_ADDSUB, 1, "iw32 ir32 ir32"},
	{"isub64", CY86_FAM_ADDSUB, 1, "iw64 ir64 ir64"},
	{"fsub32", CY86_FAM_FARITH, 1, "fw32 fr32 fr32"},
	{"fsub64", CY86_FAM_FARITH, 1, "fw64 fr64 fr64"},
	{"fsub80", CY86_FAM_FARITH, 1, "fw80 fr80 fr80"},
	{"smul8", CY86_FAM_MUL, 0, "sw8 sr8 sr8"},
	{"smul16", CY86_FAM_MUL, 0, "sw16 sr16 sr16"},
	{"smul32", CY86_FAM_MUL, 0, "sw32 sr32 sr32"},
	{"smul64", CY86_FAM_MUL, 0, "sw64 sr64 sr64"},
	{"umul8", CY86_FAM_MUL, 1, "uw8 ur8 ur8"},
	{"umul16", CY86_FAM_MUL, 1, "uw16 ur16 ur16"},
	{"umul32", CY86_FAM_MUL, 1, "uw32 ur32 ur32"},
	{"umul64", CY86_FAM_MUL, 1, "uw64 ur64 ur64"},
	{"fmul32", CY86_FAM_FARITH, 2, "fw32 fr32 fr32"},
	{"fmul64", CY86_FAM_FARITH, 2, "fw64 fr64 fr64"},
	{"fmul80", CY86_FAM_FARITH, 2, "fw80 fr80 fr80"},
	{"sdiv8", CY86_FAM_DIV, 0, "sw8 sr8 sr8"},
	{"sdiv16", CY86_FAM_DIV, 0, "sw16 sr16 sr16"},
	{"sdiv32", CY86_FAM_DIV, 0, "sw32 sr32 sr32"},
	{"sdiv64", CY86_FAM_DIV, 0, "sw64 sr64 sr64"},
	{"udiv8", CY86_FAM_DIV, 1, "uw8 ur8 ur8"},
	{"udiv16", CY86_FAM_DIV, 1, "uw16 ur16 ur16"},
	{"udiv32", CY86_FAM_DIV, 1, "uw32 ur32 ur32"},
	{"udiv64", CY86_FAM_DIV, 1, "uw64 ur64 ur64"},
	{"fdiv32", CY86_FAM_FARITH, 3, "fw32 fr32 fr32"},
	{"fdiv64", CY86_FAM_FARITH, 3, "fw64 fr64 fr64"},
	{"fdiv80", CY86_FAM_FARITH, 3, "fw80 fr80 fr80"},
	{"smod8", CY86_FAM_DIV, 2, "sw8 sr8 sr8"},
	{"smod16", CY86_FAM_DIV, 2, "sw16 sr16 sr16"},
	{"smod32", CY86_FAM_DIV, 2, "sw32 sr32 sr32"},
	{"smod64", CY86_FAM_DIV, 2, "sw64 sr64 sr64"},
	{"umod8", CY86_FAM_DIV, 3, "uw8 ur8 ur8"},
	{"umod16", CY86_FAM_DIV, 3, "uw16 ur16 ur16"},
	{"umod32", CY86_FAM_DIV, 3, "uw32 ur32 ur32"},
	{"umod64", CY86_FAM_DIV, 3, "uw64 ur64 ur64"},
	{"ieq8", CY86_FAM_ICMP, CY86_REL_EQ, "wb8 ir8 ir8"},
	{"ieq16", CY86_FAM_ICMP, CY86_REL_EQ, "wb8 ir16 ir16"},
	{"ieq32", CY86_FAM_ICMP, CY86_REL_EQ, "wb8 ir32 ir32"},
	{"ieq64", CY86_FAM_ICMP, CY86_REL_EQ, "wb8 ir64 ir64"},
	{"feq32", CY86_FAM_FCMP, CY86_REL_EQ, "wb8 fr32 fr32"},
	{"feq64", CY86_FAM_FCMP, CY86_REL_EQ, "wb8 fr64 fr64"},
	{"feq80", CY86_FAM_FCMP, CY86_REL_EQ, "wb8 fr80 fr80"},
	{"ine8", CY86_FAM_ICMP, CY86_REL_NE, "wb8 ir8 ir8"},
	{"ine16", CY86_FAM_ICMP, CY86_REL_NE, "wb8 ir16 ir16"},
	{"ine32", CY86_FAM_ICMP, CY86_REL_NE, "wb8 ir32 ir32"},
	{"ine64", CY86_FAM_ICMP, CY86_REL_NE, "wb8 ir64 ir64"},
	{"fne32", CY86_FAM_FCMP, CY86_REL_NE, "wb8 fr32 fr32"},
	{"fne64", CY86_FAM_FCMP, CY86_REL_NE, "wb8 fr64 fr64"},
	{"fne80", CY86_FAM_FCMP, CY86_REL_NE, "wb8 fr80 fr80"},
	{"slt8", CY86_FAM_ICMP, CY86_REL_LT, "wb8 sr8 sr8"},
	{"slt16", CY86_FAM_ICMP, CY86_REL_LT, "wb8 sr16 sr16"},
	{"slt32", CY86_FAM_ICMP, CY86_REL_LT, "wb8 sr32 sr32"},
	{"slt64", CY86_FAM_ICMP, CY86_REL_LT, "wb8 sr64 sr64"},
	{"ult8", CY86_FAM_ICMP, CY86_REL_LT | CY86_REL_UNSIGNED, "wb8 ur8 ur8"},
	{"ult16", CY86_FAM_ICMP, CY86_REL_LT | CY86_REL_UNSIGNED,
	 "wb8 ur16 ur16"},
	{"ult32", CY86_FAM_ICMP, CY86_REL_LT | CY86_REL_UNSIGNED,
	 "wb8 ur32 ur32"},
	{"ult64", CY86_FAM_ICMP, CY86_REL_LT | CY86_REL_UNSIGNED,
	 "wb8 ur64 ur64"},
	{"flt32", CY86_FAM_FCMP, CY86_REL_LT, "wb8 fr32 fr32"},
	{"flt64", CY86_FAM_FCMP, CY86_REL_LT, "wb8 fr64 fr64"},
	{"flt80", CY86_FAM_FCMP, CY86_REL_LT, "wb8 fr80 fr80"},
	{"sgt8", CY86_FAM_ICMP, CY86_REL_GT, "wb8 sr8 sr8"},
	{"sgt16", CY86_FAM_ICMP, CY86_REL_GT, "wb8 sr16 sr16"},
	{"sgt32", CY86_FAM_ICMP, CY86_REL_GT, "wb8 sr32 sr32"},
	{"sgt64", CY86_FAM_ICMP, CY86_REL_GT, "wb8 sr64 sr64"},
	{"ugt8", CY86_FAM_ICMP, CY86_REL_GT | CY86_REL_UNSIGNED, "wb8 ur8 ur8"},
	{"ugt16", CY86_FAM_ICMP, CY86_REL_GT | CY86_REL_UNSIGNED,
	 "wb8 ur16 ur16"},
	{"ugt32", CY86_FAM_ICMP, CY86_REL_GT | CY86_REL_UNSIGNED,
	 "wb8 ur32 ur32"},
	{"ugt64", CY86_FAM_ICMP, CY86_REL_GT | CY86_REL_UNSIGNED,
	 "wb8 ur64 ur64"},
	{"fgt32", CY86_FAM_FCMP, CY86_REL_GT, "wb8 fr32 fr32"},
	{"fgt64", CY86_FAM_FCMP, CY86_REL_GT, "wb8 fr64 fr64"},
	{"fgt80", CY86_FAM_FCMP, CY86_REL_GT, "wb8 fr80 fr80"},
	{"sle8", CY86_FAM_ICMP, CY86_REL_LE, "wb8 sr8 sr8"},
	{"sle16", CY86_FAM_ICMP, CY86_REL_LE, "wb8 sr16 sr16"},
	{"sle32", CY86_FAM_ICMP, CY86_REL_LE, "wb8 sr32 sr32"},
	{"sle64", CY86_FAM_ICMP, CY86_REL_LE, "wb8 sr64 sr64"},
	{"ule8", CY86_FAM_ICMP, CY86_REL_LE | CY86_REL_UNSIGNED, "wb8 ur8 ur8"},
	{"ule16", CY86_FAM_ICMP, CY86_REL_LE | CY86_REL_UNSIGNED,
	 "wb8 ur16 ur16"},
	{"ule32", CY86_FAM_ICMP, CY86_REL_LE | CY86_REL_UNSIGNED,
	 "wb8 ur32 ur32"},
	{"ule64", CY86_FAM_ICMP, CY86_REL_LE | CY86_REL_UNSIGNED,
	 "wb8 ur64 ur64"},
	{"fle32", CY86_FAM_FCMP, CY86_REL_LE, "wb8 fr32 fr32"},
	{"fle64", CY86_FAM_FCMP, CY86_REL_LE, "wb8 fr64 fr64"},
	{"fle80", CY86_FAM_FCMP, CY86_REL_LE, "wb8 fr80 fr80"},
	{"sge8", CY86_FAM_ICMP, CY86_REL_GE, "wb8 sr8 sr8"},
	{"sge16", CY86_FAM_ICMP, CY86_REL_GE, "wb8 sr16 sr16"},
	{"sge32", CY86_FAM_ICMP, CY86_REL_GE, "wb8 sr32 sr32"},
	{"sge64", CY86_FAM_ICMP, CY86_REL_GE, "wb8 sr64 sr64"},
	{"uge8", CY86_FAM_ICMP, CY86_REL_GE | CY86_REL_UNSIGNED, "wb8 ur8 ur8"},
	{"uge16", CY86_FAM_ICMP, CY86_REL_GE | CY86_REL_UNSIGNED,
	 "wb8 ur16 ur16"},
	{"uge32", CY86_FAM_ICMP, CY86_REL_GE | CY86_REL_UNSIGNED,
	 "wb8 ur32 ur32"},
	{"uge64", CY86_FAM_ICMP, CY86_REL_GE | CY86_REL_UNSIGNED,
	 "wb8 ur64 ur64"},
	{"fge32", CY86_FAM_FCMP, CY86_REL_GE, "wb8 fr32 fr32"},
	{"fge64", CY86_FAM_FCMP, CY86_REL_GE, "wb8 fr64 fr64"},
	{"fge80", CY86_FAM_FCMP, CY86_REL_GE, "wb8 fr80 fr80"},
	{"syscall0", CY86_FAM_SYSCALL, 0, "w64 r64"},
	{"syscall1", CY86_FAM_SYSCALL, 1, "w64 r64 r64"},
	{"syscall2", CY86_FAM_SYSCALL, 2, "w64 r64 r64 r64"},
	{"syscall3", CY86_FAM_SYSCALL, 3, "w64 r64 r64 r64 r64"},
	{"syscall4", CY86_FAM_SYSCALL, 4, "w64 r64 r64 r64 r64 r64"},
	{"syscall5", CY86_FAM_SYSCALL, 5, "w64 r64 r64 r64 r64 r64 r64"},
	{"syscall6", CY86_FAM_SYSCALL, 6, "w64 r64 r64 r64 r64 r64 r64 r64"}
};

vector<CY86OperandSpec> ParseOperandSpecs(const char* spec)
{
	vector<CY86OperandSpec> specs;
	const char* p = spec;
	while (*p)
	{
		if (*p == ' ')
		{
			p++;
			continue;
		}
		CY86OperandSpec one;
		for (; *p && *p != ' ' && (*p < '0' || *p > '9'); p++)
		{
			if (*p == 'w')
				one.written = true;
			else if (*p == 'I')
				one.imm_only = true;
		}
		for (; *p >= '0' && *p <= '9'; p++)
			one.width = one.width * 10 + (*p - '0');
		specs.push_back(one);
	}
	return specs;
}

const map<string, const CY86Opcode*>& OpcodeTable()
{
	static vector<CY86Opcode> opcodes;
	static map<string, const CY86Opcode*> table;
	if (table.empty())
	{
		size_t count = sizeof(kRawOpcodes) / sizeof(kRawOpcodes[0]);
		opcodes.resize(count);
		for (size_t i = 0; i < count; i++)
		{
			opcodes[i].name = kRawOpcodes[i].name;
			opcodes[i].family = kRawOpcodes[i].family;
			opcodes[i].variant = kRawOpcodes[i].variant;
			opcodes[i].spec = kRawOpcodes[i].spec;
			opcodes[i].operands = ParseOperandSpecs(kRawOpcodes[i].spec);
			table[opcodes[i].name] = &opcodes[i];
		}
	}
	return table;
}

// ---------------------------------------------------------------
// Register names.
// ---------------------------------------------------------------

struct RegisterName
{
	const char* name;
	ECY86Reg reg;
	int width;
};

const RegisterName kRegisters[] =
{
	{"sp", CY86_REG_SP, 64}, {"bp", CY86_REG_BP, 64},
	{"x8", CY86_REG_X, 8}, {"x16", CY86_REG_X, 16},
	{"x32", CY86_REG_X, 32}, {"x64", CY86_REG_X, 64},
	{"y8", CY86_REG_Y, 8}, {"y16", CY86_REG_Y, 16},
	{"y32", CY86_REG_Y, 32}, {"y64", CY86_REG_Y, 64},
	{"z8", CY86_REG_Z, 8}, {"z16", CY86_REG_Z, 16},
	{"z32", CY86_REG_Z, 32}, {"z64", CY86_REG_Z, 64},
	{"t8", CY86_REG_T, 8}, {"t16", CY86_REG_T, 16},
	{"t32", CY86_REG_T, 32}, {"t64", CY86_REG_T, 64}
};

bool LookupRegister(const string& name, ECY86Reg& reg, int& width)
{
	size_t count = sizeof(kRegisters) / sizeof(kRegisters[0]);
	for (size_t i = 0; i < count; i++)
	{
		if (name == kRegisters[i].name)
		{
			reg = kRegisters[i].reg;
			width = kRegisters[i].width;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------
// Literal value rules (PA2 value bytes are little-endian).
// ---------------------------------------------------------------

bool IsIntegralType(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR: case FT_SHORT_INT: case FT_INT: case FT_LONG_INT:
	case FT_LONG_LONG_INT: case FT_UNSIGNED_CHAR:
	case FT_UNSIGNED_SHORT_INT: case FT_UNSIGNED_INT:
	case FT_UNSIGNED_LONG_INT: case FT_UNSIGNED_LONG_LONG_INT:
	case FT_WCHAR_T: case FT_CHAR: case FT_CHAR16_T: case FT_CHAR32_T:
	case FT_BOOL:
		return true;
	default:
		return false;
	}
}

// char and wchar_t are signed on the x86-64 Linux target.
bool IsSignedIntegralType(EFundamentalType type)
{
	switch (type)
	{
	case FT_SIGNED_CHAR: case FT_SHORT_INT: case FT_INT: case FT_LONG_INT:
	case FT_LONG_LONG_INT: case FT_WCHAR_T: case FT_CHAR:
		return true;
	default:
		return false;
	}
}

// Arithmetic negation in place: two's complement for integral types,
// sign-bit flip for the IEEE/x87 floating encodings.
void NegateLiteral(PostToken& token)
{
	if (token.kind != PTK_LITERAL)
		throw runtime_error("'-' may only be applied to arithmetic "
		                    "literals");
	if (IsIntegralType(token.type))
	{
		int carry = 1;
		for (size_t i = 0; i < token.data.size(); i++)
		{
			int b = (~static_cast<unsigned char>(token.data[i]) & 0xFF) +
			        carry;
			token.data[i] = static_cast<char>(b & 0xFF);
			carry = b >> 8;
		}
		return;
	}
	if (token.type == FT_FLOAT)
		token.data[3] ^= '\x80';
	else if (token.type == FT_DOUBLE)
		token.data[7] ^= '\x80';
	else if (token.type == FT_LONG_DOUBLE)
		token.data[9] ^= '\x80';
	else
		throw runtime_error("'-' may only be applied to arithmetic "
		                    "literals");
}

// The PA9 operand width conversion: too long truncates the rightmost
// (most significant) bytes; too short sign-extends signed integral
// types and zero-extends everything else.
string ConvertLiteralBytes(const PostToken& token, size_t nbytes)
{
	string bytes = token.data;
	if (bytes.size() > nbytes)
		return bytes.substr(0, nbytes);
	char fill = 0;
	if (token.kind == PTK_LITERAL && IsSignedIntegralType(token.type) &&
	    !bytes.empty() &&
	    (static_cast<unsigned char>(bytes[bytes.size() - 1]) & 0x80))
		fill = '\xFF';
	bytes.append(nbytes - bytes.size(), fill);
	return bytes;
}

unsigned long long LiteralTo64(const PostToken& token)
{
	return LittleEndianValue(ConvertLiteralBytes(token, 8));
}

// Literal statements pad to the alignment of the literal's type:
// the scalar size, or the element size for string literals.
size_t LiteralAlignment(const PostToken& token)
{
	if (token.kind == PTK_LITERAL_ARRAY && token.num_elements > 0)
		return token.data.size() / token.num_elements;
	return token.data.size();
}

// ---------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------

// The parser: one forward pass over the concatenated token sequence.
// Operand immediates are width-converted as they are parsed (the
// opcode, and so each operand's constraint, is known first); label
// references stay symbolic until Finalize verifies they are defined.
class CY86Parser
{
public:
	CY86Parser(const vector<PostToken>& tokens, CY86Program& program);

	void Parse();

private:
	bool AtEnd() const;
	const PostToken& Cur() const;
	bool CurIs(ETokenType type) const;
	bool PeekIs(size_t ahead, ETokenType type) const;
	bool CurIsIdentifier() const;
	bool CurIsLiteral() const;
	void ExpectSimple(ETokenType type, const char* what);
	PostToken RequireLiteral();

	void ValidateTokens() const;
	int InternLabel(const string& name);
	void DefineLabel(const string& name);
	void ParseStatement();
	void ParseInstruction();
	void ParseDataStatement();
	CY86Operand ParseOperand(const CY86OperandSpec& spec,
	                         const string& opname);
	unsigned long long OffsetLiteral(bool minus, bool require_integral);
	CY86Operand ParseParenImmediate(const CY86OperandSpec& spec);
	CY86Operand ParseMemoryOperand();
	void Finalize();

	const vector<PostToken>& tokens_;
	CY86Program& program_;
	size_t pos_;
	map<string, int> label_ids_;
	vector<bool> label_defined_;
};

CY86Parser::CY86Parser(const vector<PostToken>& tokens,
                       CY86Program& program)
	: tokens_(tokens), program_(program), pos_(0)
{
}

void CY86Parser::Parse()
{
	ValidateTokens();
	while (pos_ < tokens_.size())
		ParseStatement();
	Finalize();
}

bool CY86Parser::AtEnd() const
{
	return pos_ >= tokens_.size();
}

const PostToken& CY86Parser::Cur() const
{
	if (AtEnd())
		throw runtime_error("unexpected end of program");
	return tokens_[pos_];
}

bool CY86Parser::CurIs(ETokenType type) const
{
	return !AtEnd() && tokens_[pos_].kind == PTK_SIMPLE &&
	       tokens_[pos_].token_type == type;
}

bool CY86Parser::PeekIs(size_t ahead, ETokenType type) const
{
	size_t at = pos_ + ahead;
	return at < tokens_.size() && tokens_[at].kind == PTK_SIMPLE &&
	       tokens_[at].token_type == type;
}

bool CY86Parser::CurIsIdentifier() const
{
	return !AtEnd() && tokens_[pos_].kind == PTK_IDENTIFIER;
}

bool CY86Parser::CurIsLiteral() const
{
	return !AtEnd() && (tokens_[pos_].kind == PTK_LITERAL ||
	                    tokens_[pos_].kind == PTK_LITERAL_ARRAY);
}

void CY86Parser::ExpectSimple(ETokenType type, const char* what)
{
	if (!CurIs(type))
		throw runtime_error(string("expected ") + what);
	pos_++;
}

PostToken CY86Parser::RequireLiteral()
{
	if (!CurIsLiteral())
		throw runtime_error("expected literal");
	PostToken token = Cur();
	pos_++;
	return token;
}

// Keywords and user-defined literals are reserved-but-unused in CY86:
// their presence anywhere makes the program ill-formed.
void CY86Parser::ValidateTokens() const
{
	for (size_t i = 0; i < tokens_.size(); i++)
	{
		const PostToken& token = tokens_[i];
		switch (token.kind)
		{
		case PTK_SIMPLE:
			if (token.token_type < OP_LBRACE)
				throw runtime_error("C++ keyword is not valid in CY86: " +
				                    token.source);
			break;
		case PTK_IDENTIFIER:
		case PTK_LITERAL:
		case PTK_LITERAL_ARRAY:
			break;
		default:
			throw runtime_error("invalid CY86 token: " + token.source);
		}
	}
}

int CY86Parser::InternLabel(const string& name)
{
	map<string, int>::iterator it = label_ids_.find(name);
	if (it != label_ids_.end())
		return it->second;
	int id = static_cast<int>(program_.label_names.size());
	label_ids_[name] = id;
	program_.label_names.push_back(name);
	program_.label_statement.push_back(0);
	label_defined_.push_back(false);
	return id;
}

void CY86Parser::DefineLabel(const string& name)
{
	ECY86Reg reg;
	int width;
	if (LookupRegister(name, reg, width))
		throw runtime_error("label collides with register name: " + name);
	if (OpcodeTable().count(name))
		throw runtime_error("label collides with opcode name: " + name);
	int id = InternLabel(name);
	if (label_defined_[static_cast<size_t>(id)])
		throw runtime_error("duplicate label: " + name);
	label_defined_[static_cast<size_t>(id)] = true;
	program_.label_statement[static_cast<size_t>(id)] =
		program_.statements.size();
}

void CY86Parser::ParseStatement()
{
	while (CurIsIdentifier() && PeekIs(1, OP_COLON))
	{
		DefineLabel(Cur().source);
		pos_ += 2;
	}
	if (CurIsIdentifier())
		ParseInstruction();
	else
		ParseDataStatement();
	ExpectSimple(OP_SEMICOLON, "';'");
}

void CY86Parser::ParseInstruction()
{
	const string& name = Cur().source;
	map<string, const CY86Opcode*>::const_iterator it =
		OpcodeTable().find(name);
	if (it == OpcodeTable().end())
		throw runtime_error("unknown opcode: " + name);
	pos_++;

	CY86Statement stmt;
	stmt.opcode = it->second;
	const vector<CY86OperandSpec>& specs = stmt.opcode->operands;
	while (!AtEnd() && !CurIs(OP_SEMICOLON))
	{
		if (stmt.operands.size() >= specs.size())
			throw runtime_error("too many operands for: " + name);
		stmt.operands.push_back(
			ParseOperand(specs[stmt.operands.size()], name));
	}
	if (stmt.operands.size() != specs.size())
		throw runtime_error("missing operands for: " + name);
	program_.statements.push_back(stmt);
}

void CY86Parser::ParseDataStatement()
{
	CY86Statement stmt;
	bool minus = CurIs(OP_MINUS);
	if (minus)
		pos_++;
	PostToken literal = RequireLiteral();
	if (minus)
		NegateLiteral(literal);
	stmt.data = literal.data;
	stmt.data_align = LiteralAlignment(literal);
	program_.statements.push_back(stmt);
}

CY86Operand CY86Parser::ParseOperand(const CY86OperandSpec& spec,
                                     const string& opname)
{
	CY86Operand op;
	if (CurIsIdentifier())
	{
		ECY86Reg reg;
		int width;
		if (LookupRegister(Cur().source, reg, width))
		{
			op.kind = CY86_OPERAND_REGISTER;
			op.reg = reg;
			op.reg_width = width;
			if (width != spec.width)
				throw runtime_error(
					"register operand width mismatch for: " + opname);
			pos_++;
		}
		else
		{
			op.kind = CY86_OPERAND_IMMEDIATE;
			op.imm.has_label = true;
			op.imm.label = InternLabel(Cur().source);
			pos_++;
		}
	}
	else if (CurIsLiteral())
	{
		op.kind = CY86_OPERAND_IMMEDIATE;
		op.imm.bytes = ConvertLiteralBytes(Cur(), spec.width / 8);
		pos_++;
	}
	else if (CurIs(OP_LPAREN))
	{
		op = ParseParenImmediate(spec);
	}
	else if (CurIs(OP_LSQUARE))
	{
		op = ParseMemoryOperand();
	}
	else
	{
		throw runtime_error("expected operand for: " + opname);
	}

	if (spec.imm_only && op.kind != CY86_OPERAND_IMMEDIATE)
		throw runtime_error("operand must be an immediate for: " + opname);
	if (spec.written && op.kind == CY86_OPERAND_IMMEDIATE)
		throw runtime_error(
			"written operand may not be an immediate for: " + opname);
	return op;
}

// The +-literal term of a label immediate or memory address, negated
// and extended to 64 bits. Label offsets require an integral literal;
// plain memory displacements accept any literal through the 64-bit
// immediate conversion.
unsigned long long CY86Parser::OffsetLiteral(bool minus,
                                             bool require_integral)
{
	PostToken literal = RequireLiteral();
	if (require_integral &&
	    (literal.kind != PTK_LITERAL || !IsIntegralType(literal.type)))
		throw runtime_error("label offset must be an integral literal");
	if (minus)
		NegateLiteral(literal);
	return LiteralTo64(literal);
}

CY86Operand CY86Parser::ParseParenImmediate(const CY86OperandSpec& spec)
{
	pos_++;  // (
	CY86Operand op;
	op.kind = CY86_OPERAND_IMMEDIATE;
	if (CurIs(OP_MINUS))
	{
		pos_++;
		PostToken literal = RequireLiteral();
		NegateLiteral(literal);
		op.imm.bytes = ConvertLiteralBytes(literal, spec.width / 8);
	}
	else if (CurIsLiteral())
	{
		op.imm.bytes = ConvertLiteralBytes(Cur(), spec.width / 8);
		pos_++;
	}
	else if (CurIsIdentifier())
	{
		ECY86Reg reg;
		int width;
		if (LookupRegister(Cur().source, reg, width))
			throw runtime_error("expected label, got register: " +
			                    Cur().source);
		op.imm.has_label = true;
		op.imm.label = InternLabel(Cur().source);
		pos_++;
		if (CurIs(OP_PLUS) || CurIs(OP_MINUS))
		{
			bool minus = CurIs(OP_MINUS);
			pos_++;
			op.imm.addend = OffsetLiteral(minus, true);
		}
	}
	else
	{
		throw runtime_error("expected immediate after '('");
	}
	ExpectSimple(OP_RPAREN, "')'");
	return op;
}

CY86Operand CY86Parser::ParseMemoryOperand()
{
	pos_++;  // [
	CY86Operand op;
	op.kind = CY86_OPERAND_MEMORY;
	if (CurIsLiteral())
	{
		op.imm.addend = LiteralTo64(Cur());
		pos_++;
	}
	else if (CurIsIdentifier())
	{
		ECY86Reg reg;
		int width;
		bool is_register = LookupRegister(Cur().source, reg, width);
		if (is_register)
		{
			if (width != 64)
				throw runtime_error(
					"memory address register must be 64-bit: " +
					Cur().source);
			op.mem_has_base = true;
			op.mem_base = reg;
		}
		else
		{
			op.imm.has_label = true;
			op.imm.label = InternLabel(Cur().source);
		}
		pos_++;
		if (CurIs(OP_PLUS) || CurIs(OP_MINUS))
		{
			bool minus = CurIs(OP_MINUS);
			pos_++;
			op.imm.addend = OffsetLiteral(minus, !is_register);
		}
	}
	else
	{
		throw runtime_error("expected memory address after '['");
	}
	ExpectSimple(OP_RSQUARE, "']'");
	return op;
}

void CY86Parser::Finalize()
{
	for (size_t s = 0; s < program_.statements.size(); s++)
	{
		const vector<CY86Operand>& operands =
			program_.statements[s].operands;
		for (size_t i = 0; i < operands.size(); i++)
		{
			const CY86Immediate& imm = operands[i].imm;
			if (imm.has_label &&
			    !label_defined_[static_cast<size_t>(imm.label)])
				throw runtime_error(
					"use of undefined label: " +
					program_.label_names[static_cast<size_t>(imm.label)]);
		}
	}
	map<string, int>::iterator it = label_ids_.find("start");
	if (it != label_ids_.end() &&
	    label_defined_[static_cast<size_t>(it->second)])
		program_.start_label = it->second;
}

}  // namespace

void ParseCY86Program(const vector<PostToken>& tokens, CY86Program& program)
{
	CY86Parser parser(tokens, program);
	parser.Parse();
}
