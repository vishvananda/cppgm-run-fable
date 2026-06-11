#pragma once

#include <string>

using std::string;

#include "sema/scope.h"
#include "sema/sem_node.h"
#include "sema/type.h"

// PA14 constant evaluation over analyzed initializer trees: global
// definitions need folded scalar values, address constants, and
// string-literal references, all computed from the typed semantic
// nodes (never from printed text).

enum ELowerConstKind
{
	LC_INT,     // typed integer value (covers bool and enums)
	LC_FLOAT,   // floating literal spelling
	LC_NULL,    // null pointer
	LC_ADDR,    // address of a namespace-scope object or function
	LC_STRING   // string-literal object
};

struct LowerConst
{
	LowerConst()
		: kind(LC_INT), entity_scope(0), offset(0), string_node(0)
	{}

	ELowerConstKind kind;
	ConstValue ival;          // LC_INT
	string float_token;       // LC_FLOAT source spelling
	TypePtr float_type;       // LC_FLOAT
	const Scope* entity_scope;  // LC_ADDR declaring scope
	string entity_name;         // LC_ADDR declared name
	TypePtr entity_type;        // LC_ADDR declared type
	long long offset;           // LC_ADDR byte offset
	const SemNode* string_node;  // LC_STRING literal node
};

// Evaluates `node` as a PA14 constant initializer expression; returns
// false outside the supported constant subset.
bool EvaluateLowerConst(const SemNode& node, LowerConst& out);
