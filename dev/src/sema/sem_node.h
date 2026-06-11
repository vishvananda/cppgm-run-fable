#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

using std::ostream;
using std::string;
using std::unique_ptr;
using std::vector;

#include "sema/scope.h"
#include "sema/type.h"

// The PA12 semantic dump tree: one kind-tagged node per printed line,
// holding the resolved facts (entity name, canonical type, value
// category, operator token) as typed fields. The binder and the
// expression analyzer build the tree in declaration order; the printer
// renders it without consulting the AST or the scope model again.

enum EValueCategory
{
	VC_LVALUE,
	VC_XVALUE,
	VC_PRVALUE
};

const char* ValueCategoryName(EValueCategory category);

enum ESemNodeKind
{
	// declaration items
	SN_TYPE_ALIAS,            // type-alias <name> <type>
	SN_VARIABLE,              // variable <name> <type> [init subtree]
	SN_FUNCTION_DECLARATION,  // function-declaration <name> <type>
	SN_FUNCTION_DEFINITION,   // function-definition <name> <type>
	SN_NAMESPACE_DEFINITION,  // namespace-definition <name>
	SN_PARAMETER,             // parameter <name> <type>
	SN_CONSTRUCTOR_ACTION,    // constructor-action <name>

	// statements
	SN_COMPOUND_STATEMENT,
	SN_SIMPLE_DECLARATION,
	SN_EXPRESSION_STATEMENT,
	SN_RETURN_STATEMENT,
	SN_IF_STATEMENT,
	SN_THEN,
	SN_ELSE,
	SN_WHILE_STATEMENT,
	SN_DO_STATEMENT,
	SN_FOR_STATEMENT,
	SN_FOR_INIT,
	SN_ITERATION,
	SN_CONDITION,
	SN_CONDITION_DECLARATION,
	SN_SWITCH_STATEMENT,
	SN_CASE_STATEMENT,
	SN_DEFAULT_STATEMENT,
	SN_BREAK_STATEMENT,
	SN_CONTINUE_STATEMENT,
	SN_GOTO_STATEMENT,    // PA14: name is the target label
	SN_LABEL_STATEMENT,   // PA14: name is the label, child the statement

	// expressions: <word> <value-category> <type> [extras]
	SN_LITERAL,                // extra: value token
	SN_ID_EXPRESSION,          // extra: name as written
	SN_CALL_EXPRESSION,
	SN_CALLEE,                 // callee <name> <type> (no category)
	SN_UNARY_EXPRESSION,       // extra: TOKEN:spelling
	SN_BINARY_EXPRESSION,      // extra: TOKEN:spelling
	SN_POSTFIX_EXPRESSION,     // extra: TOKEN:spelling
	SN_ASSIGNMENT_EXPRESSION,  // extra: TOKEN:spelling
	SN_CONDITIONAL_EXPRESSION,
	SN_SUBSCRIPT_EXPRESSION,
	SN_MEMBER_EXPRESSION,      // extra: [TOKEN:]member-name
	SN_CAST_EXPRESSION,        // extra: TOKEN:spelling (absent for
	                           // functional casts)
	SN_SIZEOF_EXPRESSION,
	SN_BRACED_INIT_LIST
};

struct SemNode;
typedef unique_ptr<SemNode> SemNodePtr;

struct SemNode
{
	explicit SemNode(ESemNodeKind kind_in);

	ESemNodeKind kind;
	string name;          // item / callee / id-expression / member name
	TypePtr type;         // items and expressions (null for statements)
	EValueCategory category;
	bool has_op;          // operator annotation present
	ETokenType op;
	string op_spelling;   // text after the colon
	string token;         // SN_LITERAL value spelling
	vector<SemNodePtr> children;

	// PA14 lowering facts (never printed by the PA12 dump). The named
	// entity's identity is its declaring scope plus declared name; both
	// are stable because scopes are arena-owned by the TypesModel.
	const Scope* entity_scope;
	string entity_name;
	bool has_value;       // decoded constant: literals, enumerator uses,
	                      // sizeof results, folded case values
	ConstValue value;
	bool null_pointer;    // null pointer literal (possibly retyped)
	bool is_string_literal;
	string string_bytes;  // string literal object representation
	// SN_VARIABLE / SN_FUNCTION_DECLARATION / SN_FUNCTION_DEFINITION
	bool is_static_decl;
	bool is_extern_decl;
	bool is_thread_local_decl;
	bool c_linkage;
	bool unwind_no;       // simple noexcept marking on the declarator
};

SemNodePtr MakeSemNode(ESemNodeKind kind);

// One translation unit's dump: the declaration-order items plus the
// implicit member functions synthesized while analyzing them (printed
// after the items, in first-need order).
struct SemUnit
{
	vector<SemNodePtr> items;
	vector<SemNodePtr> synthesized;
};

// Writes the `translation-unit` line and the tree below it.
void PrintSemanticsOutput(const SemUnit& unit, ostream& out);
