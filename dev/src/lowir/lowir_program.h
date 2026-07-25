#pragma once

#include <map>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::vector;

// PA13 LowIR object model. The parser builds one LowIRProgram from the
// concatenated command-line translation units; the validator checks the
// structural contract; the PA13 emitter translates it to CY86 text.
// Later LowIR assignments (emit-lowir, lowiropt, lowir2native) reuse this
// model as the stable backend boundary.

enum ELowIRTypeKind
{
	LOWIR_TYPE_VOID,
	LOWIR_TYPE_I1,
	LOWIR_TYPE_I8,
	LOWIR_TYPE_U8,
	LOWIR_TYPE_I16,
	LOWIR_TYPE_U16,
	LOWIR_TYPE_I32,
	LOWIR_TYPE_U32,
	LOWIR_TYPE_I64,
	LOWIR_TYPE_I128,
	LOWIR_TYPE_F32,
	LOWIR_TYPE_F64,
	LOWIR_TYPE_F80,
	LOWIR_TYPE_PTR,
	LOWIR_TYPE_OBJ
};

struct LowIRType
{
	ELowIRTypeKind kind = LOWIR_TYPE_VOID;
	long obj_bytes = 0;
	long obj_align = 0;

	bool is_void() const { return kind == LOWIR_TYPE_VOID; }
	bool is_obj() const { return kind == LOWIR_TYPE_OBJ; }
	bool is_f80() const { return kind == LOWIR_TYPE_F80; }

	bool is_integer() const
	{
		return kind >= LOWIR_TYPE_I1 && kind <= LOWIR_TYPE_I128;
	}

	bool is_float() const
	{
		return kind == LOWIR_TYPE_F32 || kind == LOWIR_TYPE_F64 ||
		       kind == LOWIR_TYPE_F80;
	}

	// Conceptual integer bit width used by convert validation (i1 is 1).
	long integer_bits() const;

	// Register/memory operand width in bits for CY86 ops (i1 acts as 8).
	long operand_bits() const;

	// Bytes one `index` element advances the base pointer.
	long element_bytes() const;

	// Bytes of stack home backing a value of this type (8-byte granules).
	long home_bytes() const;
};

// Operand of an instruction or initializer. Literals keep their source
// spelling (without sign); the parser folds an OP_MINUS prefix into
// `negated`. `nullptr` lexes as a literal spelled "nullptr".
enum ELowIROperandKind
{
	LOWIR_OPERAND_NONE,
	LOWIR_OPERAND_TEMP,    // %name
	LOWIR_OPERAND_SLOT,    // $name
	LOWIR_OPERAND_GLOBAL,  // @name (global or function symbol)
	LOWIR_OPERAND_LITERAL
};

enum ELowIRLiteralClass
{
	LOWIR_LITERAL_INT,
	LOWIR_LITERAL_F32,    // 1.5f
	LOWIR_LITERAL_F64,    // 2.25
	LOWIR_LITERAL_F80,    // 1.25L
	LOWIR_LITERAL_NULLPTR
};

struct LowIROperand
{
	ELowIROperandKind kind = LOWIR_OPERAND_NONE;
	string name;                  // temp/slot/global spelling without sigil
	string literal;               // literal spelling without sign
	bool negated = false;
	ELowIRLiteralClass literal_class = LOWIR_LITERAL_INT;

	bool is_literal() const { return kind == LOWIR_OPERAND_LITERAL; }
};

// Ordered metadata items ([key=value, ...]); order is kept for diagnostics
// but lookups go through find().
struct LowIRMetadata
{
	vector<std::pair<string, string>> items;

	bool has(const string & key) const;
	string find(const string & key) const;  // "" when absent
};

enum ELowIROpcode
{
	LOWIR_INS_CONST,
	LOWIR_INS_COPY,
	LOWIR_INS_ADDR,
	LOWIR_INS_LOAD,
	LOWIR_INS_STORE,
	LOWIR_INS_ATOMIC_LOAD,
	LOWIR_INS_ATOMIC_STORE,
	LOWIR_INS_ATOMIC_EXCHANGE,
	LOWIR_INS_ATOMIC_COMPARE_EXCHANGE,
	LOWIR_INS_ATOMIC_ADD_FETCH,
	LOWIR_INS_ATOMIC_THREAD_FENCE,
	LOWIR_INS_ATOMIC_SIGNAL_FENCE,
	LOWIR_INS_INDEX,
	LOWIR_INS_UNARY,
	LOWIR_INS_BINARY,
	LOWIR_INS_CMP,
	LOWIR_INS_CONVERT,
	LOWIR_INS_CALL,
	LOWIR_INS_COPYOBJ,
	LOWIR_INS_ZEROINIT,
	LOWIR_INS_EXCEPTION,
	LOWIR_INS_EH_TRY,
	LOWIR_INS_EH_CLEANUP,
	LOWIR_INS_EH_END,
	LOWIR_INS_EH_MARKER,   // eh_catch / eh_filter / eh_catch_all
	LOWIR_INS_THROW,
	LOWIR_INS_RESUME,
	LOWIR_INS_JUMP,
	LOWIR_INS_BRANCH,
	LOWIR_INS_SWITCH,
	LOWIR_INS_RETURN
};

struct LowIRParam
{
	string name;            // %name spelling without sigil
	LowIRType type;
	LowIRMetadata metadata;
};

// Explicit `as (...) -> type [...]` signature on indirect calls.
struct LowIRCallSignature
{
	bool present = false;
	vector<LowIRParam> params;
	LowIRType return_type;
	LowIRMetadata metadata;
};

// Optional `!dbg(file, line, column)` source location suffix. Carried
// through backend lowering so machine-IR instructions keep their
// source positions (the PA38 debug-metadata contract).
struct LowIRDebugLocation
{
	string file;
	long line = 0;
	long column = 0;

	bool present() const
	{
		return !file.empty() && line > 0 && column > 0;
	}
};

struct LowIRInstruction
{
	ELowIROpcode opcode = LOWIR_INS_RETURN;
	string result;            // destination temp, "" for none
	LowIRType type;           // primary written type
	LowIRType type2;          // convert source type
	string operation;         // unary/binary op, cmp predicate, convert op
	LowIRMetadata metadata;   // index projection metadata
	vector<LowIROperand> operands;
	string callee;            // direct callee spelling for calls
	bool callee_is_temp = false;
	LowIRCallSignature signature;
	vector<string> block_targets;          // jump/branch/eh/switch labels
	vector<LowIROperand> switch_values;    // case values, parallel to
	                                       // block_targets[1..]
	long span_bytes = 0;      // copyobj/zeroinit byte count
	long span_align = 0;      // copyobj/zeroinit alignment
	long atomic_order = 5;    // atomic memory order (__ATOMIC_* value)
	long atomic_failure_order = 5;
	vector<string> eh_types;  // eh_catch/eh_filter type symbols
	long eh_selector = 0;     // eh_catch/eh_catch_all handler selector
	LowIRDebugLocation debug_location;

	bool is_terminator() const
	{
		return opcode == LOWIR_INS_JUMP || opcode == LOWIR_INS_BRANCH ||
		       opcode == LOWIR_INS_SWITCH || opcode == LOWIR_INS_RETURN ||
		       opcode == LOWIR_INS_THROW || opcode == LOWIR_INS_RESUME;
	}
};

struct LowIRBlock
{
	string label;             // ^label spelling without sigil
	vector<LowIRInstruction> instructions;
};

struct LowIRSlot
{
	string name;              // $name spelling without sigil
	LowIRType type;
};

struct LowIRFunction
{
	string name;              // @name spelling without sigil
	bool is_definition = false;
	vector<LowIRParam> params;
	LowIRType return_type;
	LowIRMetadata metadata;
	vector<LowIRSlot> slots;
	vector<LowIRBlock> blocks;
};

enum ELowIRDataItemKind
{
	LOWIR_DATA_SCALAR,   // <type> <literal>
	LOWIR_DATA_ADDR,     // ptr addr @symbol (+/- addend)
	LOWIR_DATA_ZERO      // zero <bytes>
};

struct LowIRDataItem
{
	ELowIRDataItemKind kind = LOWIR_DATA_ZERO;
	LowIRType type;
	LowIROperand value;       // scalar literal
	string symbol;            // addr target
	long addend = 0;
	bool has_addend = false;
	long zero_bytes = 0;
};

enum ELowIRGlobalInit
{
	LOWIR_GLOBAL_NONE,       // declaration only
	LOWIR_GLOBAL_ZERO,
	LOWIR_GLOBAL_SCALAR,
	LOWIR_GLOBAL_ADDR,
	LOWIR_GLOBAL_STRUCTURED
};

struct LowIRGlobal
{
	string name;              // @name spelling without sigil
	bool is_definition = false;
	bool has_type = false;
	LowIRType type;
	LowIRMetadata metadata;
	ELowIRGlobalInit init = LOWIR_GLOBAL_NONE;
	LowIROperand scalar;      // scalar initializer
	string addr_symbol;       // addr initializer target
	long addr_addend = 0;
	bool has_addr_addend = false;
	vector<LowIRDataItem> items;
};

struct LowIRAlias
{
	string object_symbol;
	string target;            // @name spelling without sigil
};

// Top-level entries in source order; `order` interleaves the three lists
// so emission can follow declaration order per kind.
enum ELowIRTopLevelKind
{
	LOWIR_TOP_GLOBAL,
	LOWIR_TOP_FUNCTION,
	LOWIR_TOP_ALIAS
};

struct LowIRProgram
{
	vector<LowIRGlobal> globals;
	vector<LowIRFunction> functions;
	vector<LowIRAlias> aliases;
};

// Parses the LowIR type spelling ("i64", "obj<16x8>", ...); throws
// std::runtime_error on unknown spellings.
LowIRType ParseLowIRType(const string & spelling);
