#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

using std::map;
using std::size_t;
using std::string;
using std::vector;

#include "post_token.h"

// PA9 CY86 object model and parser. The phase-7 token sequences of
// all translation units, concatenated in command-line order, are
// matched against pa9.gram; the result is a CY86Program of statements
// with typed operands plus the label table. All diagnosable
// ill-formedness (keywords or user-defined literals anywhere, label
// spelling collisions, unknown opcodes, operand arity/kind/width
// violations, non-integral label offsets, undefined labels) throws
// std::runtime_error, which the cy86 tool reports as EXIT_FAILURE.

// Operand constraint families drive the CY86-to-x86 translation; the
// variant distinguishes opcodes within a family (see cy86_parser.cpp).
enum ECY86Family
{
	CY86_FAM_DATA,      // data8..data64: immediate bytes in the image
	CY86_FAM_MOVE,      // move8..move80
	CY86_FAM_JUMP,
	CY86_FAM_JUMPIF,
	CY86_FAM_CALL,
	CY86_FAM_RET,
	CY86_FAM_NOT,
	CY86_FAM_BITWISE,   // variant: 0 and, 1 or, 2 xor
	CY86_FAM_SHIFT,     // variant: 0 lshift, 1 srshift, 2 urshift
	CY86_FAM_ADDSUB,    // variant: 0 iadd, 1 isub
	CY86_FAM_MUL,       // variant: 0 signed, 1 unsigned
	CY86_FAM_DIV,       // variant: 0 sdiv, 1 udiv, 2 smod, 3 umod
	CY86_FAM_FARITH,    // variant: 0 add, 1 sub, 2 mul, 3 div
	CY86_FAM_ICMP,      // variant: ECY86Relation (+CY86_REL_UNSIGNED)
	CY86_FAM_FCMP,      // variant: ECY86Relation
	CY86_FAM_TO_F80,    // variant: 0 signed int, 1 unsigned int, 2 float
	CY86_FAM_FROM_F80,  // variant: 0 signed int, 1 unsigned int, 2 float
	CY86_FAM_SYSCALL    // variant: parameter count
};

// Comparison relations; ICMP variants add CY86_REL_UNSIGNED when the
// operands compare as unsigned integers.
enum ECY86Relation
{
	CY86_REL_EQ = 0,
	CY86_REL_NE = 1,
	CY86_REL_LT = 2,
	CY86_REL_GT = 3,
	CY86_REL_LE = 4,
	CY86_REL_GE = 5
};
const int CY86_REL_UNSIGNED = 8;

// One operand constraint parsed from the cy86-opcode.desc string
// (for example "sw16" or "rI8").
struct CY86OperandSpec
{
	CY86OperandSpec() : written(false), imm_only(false), width(0) {}

	bool written;   // 'w': may not be an immediate
	bool imm_only;  // 'I': must be an immediate
	int width;      // bits: 8/16/32/64/80
};

struct CY86Opcode
{
	const char* name;
	ECY86Family family;
	int variant;
	const char* spec;  // space-separated operand constraints
	vector<CY86OperandSpec> operands;
};

enum ECY86Reg
{
	CY86_REG_SP,
	CY86_REG_BP,
	CY86_REG_X,
	CY86_REG_Y,
	CY86_REG_Z,
	CY86_REG_T
};

enum ECY86OperandKind
{
	CY86_OPERAND_REGISTER,
	CY86_OPERAND_IMMEDIATE,
	CY86_OPERAND_MEMORY
};

// An immediate value: either label + addend (resolved at layout, then
// truncated to the operand width) or a width-converted literal.
struct CY86Immediate
{
	CY86Immediate() : has_label(false), label(0), addend(0) {}

	bool has_label;
	int label;
	unsigned long long addend;
	string bytes;  // literal form: operand-width value bytes
};

struct CY86Operand
{
	CY86Operand()
		: kind(CY86_OPERAND_REGISTER), reg(CY86_REG_SP), reg_width(64),
		  mem_has_base(false), mem_base(CY86_REG_SP)
	{}

	ECY86OperandKind kind;

	// CY86_OPERAND_REGISTER
	ECY86Reg reg;
	int reg_width;

	// CY86_OPERAND_IMMEDIATE: the value; CY86_OPERAND_MEMORY: the
	// address term (literal offsets already folded into addend).
	CY86Immediate imm;

	// CY86_OPERAND_MEMORY
	bool mem_has_base;
	ECY86Reg mem_base;
};

// Either an instruction (opcode != 0) or a literal data statement.
struct CY86Statement
{
	CY86Statement() : opcode(0), data_align(1) {}

	const CY86Opcode* opcode;
	vector<CY86Operand> operands;
	string data;        // literal statement: PA2 value bytes
	size_t data_align;  // literal statement: type alignment
};

struct CY86Program
{
	CY86Program() : start_label(-1) {}

	vector<CY86Statement> statements;
	vector<string> label_names;          // label id -> spelling
	vector<size_t> label_statement;      // label id -> statement index
	int start_label;                     // label id of `start`, or -1
};

// Parses the concatenated token sequence into `program`.
void ParseCY86Program(const vector<PostToken>& tokens, CY86Program& program);
