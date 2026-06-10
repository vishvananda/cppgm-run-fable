#pragma once

#include <memory>
#include <string>
#include <vector>

using std::shared_ptr;
using std::string;
using std::vector;

#include "post_token.h"

// PA7 type model: immutable nodes shared by TypePtr, so typedefs,
// redeclarations, and composed types alias freely. The factories
// enforce the structural rules of clause 8 once (cv distribution over
// arrays, cv ignored on references, reference collapsing), so a node
// never needs fixing up after construction. Array completion
// (8.3.4p22) replaces a variable's TypePtr rather than mutating the
// node, so older references to the incomplete type stay valid.

enum ETypeKind
{
	TK_FUNDAMENTAL,
	TK_POINTER,
	TK_LVALUE_REFERENCE,
	TK_RVALUE_REFERENCE,
	TK_ARRAY,
	TK_FUNCTION
};

struct Type;
typedef shared_ptr<const Type> TypePtr;

struct Type
{
	Type()
		: kind(TK_FUNDAMENTAL), is_const(false), is_volatile(false),
		  fundamental(FT_VOID), bound_known(false), bound(0),
		  variadic(false)
	{}

	ETypeKind kind;

	// cv-qualification of this level. Never set on references (8.3.2p1)
	// or arrays (8.3.4p1 stores cv on the element type instead).
	bool is_const;
	bool is_volatile;

	EFundamentalType fundamental;  // TK_FUNDAMENTAL
	TypePtr target;                // pointee / referee / element / return
	bool bound_known;              // TK_ARRAY: false for unknown bound
	unsigned long long bound;      // TK_ARRAY when bound_known
	vector<TypePtr> parameters;    // TK_FUNCTION, already adjusted (8.3.5p5)
	bool variadic;                 // TK_FUNCTION
};

TypePtr MakeFundamentalType(EFundamentalType fundamental);
TypePtr MakePointerType(const TypePtr& pointee, bool is_const,
                        bool is_volatile);

// Applies reference collapsing (8.3.2p6): && of && is &&, every other
// combination is &.
TypePtr MakeReferenceType(const TypePtr& target, bool is_rvalue);

TypePtr MakeArrayType(const TypePtr& element, bool bound_known,
                      unsigned long long bound);
TypePtr MakeFunctionType(const TypePtr& return_type,
                         const vector<TypePtr>& parameters, bool variadic);

// cv applied through a typedef-name or specifier-seq: redistributes
// onto array element types and is silently dropped on references.
TypePtr MakeCvQualifiedType(const TypePtr& type, bool add_const,
                            bool add_volatile);

// 8.3.5p5: array of T becomes pointer to T, function becomes pointer
// to function, top-level cv-qualifiers are deleted.
TypePtr AdjustParameterType(const TypePtr& type);

// Redeclaration merge: a redeclaration may complete an array of
// unknown bound; otherwise the first declared type stands (the program
// is well-formed by the PA7 contract, so they agree).
TypePtr MergeRedeclaredType(const TypePtr& existing,
                            const TypePtr& redeclared);

// Renders the recursive PA7 description ("pointer to const char",
// "function of (int, ...) returning void", ...).
string DescribeType(const TypePtr& type);
