#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "post_token.h"
#include "sema/entity.h"

// PA8 expression annotation (handout "Design Notes"): every expression
// gets a type, a value category (3.10), and a translation-time value.
// A value is either fully known (arithmetic constant, null pointer),
// known position-independently (an image object plus a byte addend -
// the symbolic form that relocation completes), or unknown until
// runtime (CK_NONCONST) - which is not an error by itself: such an
// initializer just leaves the mock object zero-initialized, while
// contexts that require constants (5.19) reject it.

enum EConstKind
{
	CK_NONCONST,  // not a constant expression / unknown until runtime
	CK_INT,       // integral value (signedness from the type)
	CK_FLOAT,     // floating value
	CK_ADDRESS,   // image object + addend (completed by relocation)
	CK_NULL       // null pointer / nullptr_t value
};

struct ConstValue
{
	ConstValue()
		: kind(CK_NONCONST), int_value(0), float_value(0.0L), target(0),
		  addend(0)
	{}

	EConstKind kind;
	unsigned long long int_value;  // CK_INT: 64-bit object representation
	long double float_value;       // CK_FLOAT
	const ImageSlot* target;       // CK_ADDRESS
	unsigned long long addend;
};

enum EExprCategory
{
	XC_PRVALUE,
	XC_LVALUE,
	XC_XVALUE  // not reachable in PA8; accounted for per the handout
};

struct Expr
{
	Expr() : category(XC_PRVALUE), object(0) {}

	// Annotated type. For lvalues naming a reference this is the
	// referenced type (references are implicitly dereferenced). Null
	// only while an overload set is unresolved.
	TypePtr type;
	EExprCategory category;
	// prvalue: the value. lvalue: the address of the designated object
	// (CK_ADDRESS, or CK_NONCONST when unknown, e.g. through an
	// undefined extern reference).
	ConstValue value;
	// lvalue: the designated object when known (carries the const/
	// constexpr/initializer facts that gate constant reads).
	const ImageSlot* object;
	// id-expression naming functions: the candidate set (13.4 resolves
	// against the destination type).
	vector<DeclaredEntity*> overloads;
};

// --- annotation of primaries ---

Expr MakeLiteralExpr(const PostToken& token);     // PTK_LITERAL
Expr MakeBoolLiteralExpr(bool value);             // true / false
Expr MakeNullptrLiteralExpr();                    // nullptr
Expr MakeStringLiteralExpr(const ImageSlot* slot);
// A variable or reference id-expression; `current_tu` gates the
// visibility of the reference's binding (5.19).
Expr MakeVariableExpr(const DeclaredEntity* entity, int current_tu);
Expr MakeFunctionExpr(const vector<DeclaredEntity*>& overloads);

// --- conversions ---

// 4.10p1: integral constant expression prvalue evaluating to zero, or
// a prvalue of nullptr_t type.
bool IsNullPointerConstant(const Expr& expr);

// 5.19 lvalue-to-rvalue: the value if the designated object may be
// read in a constant expression (non-volatile const integral with a
// visible constant initialization, or a constexpr object), CK_NONCONST
// otherwise.
ConstValue ReadLValue(const Expr& expr, int current_tu);

// The standard conversion sequence of copy-initialization (clause 4,
// 8.5p17) from `src` to scalar type `dest` (cv ignored). Decays
// lvalues first (4.1-4.3), resolving overload sets against pointer-to-
// function destinations (13.4). Returns false when no conversion
// sequence exists (ill-formed); a valid sequence over an unknown value
// yields CK_NONCONST. `out` is the converted prvalue.
bool ConvertToType(const TypePtr& dest, const Expr& src, int current_tu,
                   Expr& out);

// The object representation (little-endian value bytes) of a known
// constant of fundamental/pointer type; for CK_NULL, size bytes of
// zero. CK_ADDRESS is representable only by relocation, not bytes.
string EncodeConstant(const TypePtr& type, const ConstValue& value);

// 13.4: the member of the overload set whose type exactly matches
// `function_type`; null when none or more than one matches.
DeclaredEntity* ResolveOverloadSet(const vector<DeclaredEntity*>& set,
                                   const TypePtr& function_type);
