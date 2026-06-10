#pragma once

#include <memory>
#include <string>
#include <vector>

using std::shared_ptr;
using std::string;
using std::vector;

#include "post_token.h"

// PA7/PA8 type model: immutable nodes shared by TypePtr, so typedefs,
// redeclarations, and composed types alias freely. The factories
// enforce the structural rules of clause 8 once (cv distribution over
// arrays, cv ignored on references), so a node never needs fixing up
// after construction. Since PA8 must diagnose ill-formed programs, the
// factories throw on structurally ill-formed compositions
// (pointer-to-reference, reference-to-void, invalid array element
// types); reference collapsing (8.3.2p6) is only legal through a
// typedef-name, which the declarator composition decides. Array
// completion (8.3.4p22) replaces a variable's TypePtr rather than
// mutating the node, so older references to the incomplete type stay
// valid.

enum ETypeKind
{
	TK_FUNDAMENTAL,
	TK_POINTER,
	TK_LVALUE_REFERENCE,
	TK_RVALUE_REFERENCE,
	TK_ARRAY,
	TK_FUNCTION,
	// PA11 named types: classes, enumerations, and template type /
	// template-template parameters. Each named type points at one
	// NamedTypeInfo record; identity is pointer equality.
	TK_CLASS,
	TK_ENUM,
	TK_TYPE_PARAM
};

// One record per named-type entity, owned by the semantic model that
// declared it and shared by every Type node naming it, so completing
// the entity (class definition, enum definition) is visible through
// types composed before the completion (3.9p7).
struct NamedTypeInfo
{
	NamedTypeInfo()
		: complete(false), is_union(false), is_scoped(false),
		  is_defined(false), enum_underlying(FT_INT), size(0), alignment(0)
	{}

	string display;  // canonical spelling: "struct C", "enum class FY",
	                 // "typename T", "template-parameter TT"
	bool complete;
	bool is_union;   // classes
	// Enumeration facts (7.2): scoped-ness and the (PA11 int-fixed)
	// underlying type are redeclaration-checked entity properties, and
	// is_defined records that the enumerator list has been seen.
	bool is_scoped;
	bool is_defined;
	EFundamentalType enum_underlying;
	unsigned long long size;       // valid when complete
	unsigned long long alignment;  // valid when complete
};

struct Type;
typedef shared_ptr<const Type> TypePtr;

struct Type
{
	Type()
		: kind(TK_FUNDAMENTAL), is_const(false), is_volatile(false),
		  fundamental(FT_VOID), bound_known(false), bound(0),
		  variadic(false), named(0)
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
	const NamedTypeInfo* named;    // TK_CLASS / TK_ENUM / TK_TYPE_PARAM
};

// --- classification of fundamental types (3.9.1) ---

bool IsIntegralFundamental(EFundamentalType type);
// Signedness of the object representation on the x86-64 ABI (char and
// wchar_t are signed).
bool IsSignedIntegralFundamental(EFundamentalType type);
bool IsFloatingFundamental(EFundamentalType type);

bool IsArithmeticType(const TypePtr& type);  // integral or floating
bool IsIntegralType(const TypePtr& type);
bool IsReferenceType(const TypePtr& type);
bool IsNullPtrType(const TypePtr& type);
bool IsVoidType(const TypePtr& type);  // possibly cv-qualified void

// --- factories ---

TypePtr MakeFundamentalType(EFundamentalType fundamental);

// A class, enum, or template-parameter type naming `info` (which must
// outlive every type composed over it).
TypePtr MakeNamedType(ETypeKind kind, const NamedTypeInfo* info);

// Throws on pointer to reference (8.3.2p4).
TypePtr MakePointerType(const TypePtr& pointee, bool is_const,
                        bool is_volatile);

// Throws on reference to (cv) void. When `target` is itself a
// reference, collapses per 8.3.2p6 (&& of && is &&, every other
// combination is &) if `allow_collapse` - the caller asserts the inner
// reference came from a typedef-name - and throws otherwise (a direct
// reference-to-reference declarator is ill-formed, 8.3.2p1).
TypePtr MakeReferenceType(const TypePtr& target, bool is_rvalue,
                          bool allow_collapse);

// Throws when the element type is a reference, function, void, or an
// array of unknown bound (8.3.4p1: incomplete element type).
TypePtr MakeArrayType(const TypePtr& element, bool bound_known,
                      unsigned long long bound);

// Throws when the return type is a function or array (8.3.5p8).
TypePtr MakeFunctionType(const TypePtr& return_type,
                         const vector<TypePtr>& parameters, bool variadic);

// cv applied through a typedef-name or specifier-seq: redistributes
// onto array element types and is silently dropped on references.
TypePtr MakeCvQualifiedType(const TypePtr& type, bool add_const,
                            bool add_volatile);

// 8.3.5p5: array of T becomes pointer to T, function becomes pointer
// to function, top-level cv-qualifiers are deleted.
TypePtr AdjustParameterType(const TypePtr& type);

// Redeclaration merge: the types shall be identical except that a
// redeclaration may complete (or re-leave incomplete) an array of
// unknown bound; any other disagreement throws.
TypePtr MergeRedeclaredType(const TypePtr& existing,
                            const TypePtr& redeclared);

// --- queries ---

// Structural equality, including cv at every level.
bool TypeEquals(const TypePtr& a, const TypePtr& b);

// The type without its top-level cv-qualifiers; for arrays the
// element's cv is removed instead (3.9.3p5).
TypePtr RemoveTopCv(const TypePtr& type);

// Top-level cv (for arrays: the element's, recursively).
void TopCv(const TypePtr& type, bool& is_const, bool& is_volatile);

// 4.4: pointer to cv1 T -> pointer to cv2 T conversion validity.
bool QualificationConvertible(const TypePtr& from, const TypePtr& to);

// Size and alignment per the PA8 handout ABI table. Throws on
// incomplete types (void, array of unknown bound) and function types
// (callers emit the fixed 4-byte mock stub instead); array sizes are
// overflow-checked (8.3.4: exceeding implementation limits is
// ill-formed).
unsigned long long TypeSize(const TypePtr& type);
unsigned long long TypeAlignment(const TypePtr& type);

// Renders the recursive PA7 description ("pointer to const char",
// "function of (int, ...) returning void", ...).
string DescribeType(const TypePtr& type);

// --- simple-type-specifier combination (7.1.6.2p3) ---

// Occurrence counts of the simple-type-specifiers of one
// decl-specifier-seq, shared by the PA7 token-driven declaration parser
// and the PA11 AST type builder.
struct SimpleTypeSpecifiers
{
	SimpleTypeSpecifiers()
		: has_base(false), base(KW_INT), signed_count(0),
		  unsigned_count(0), short_count(0), long_count(0)
	{}

	bool has_base;
	ETokenType base;  // char/char16_t/char32_t/wchar_t/bool/int/
	                  // float/double/void
	int signed_count;
	int unsigned_count;
	int short_count;
	int long_count;
};

bool AnySimpleTypeSpecifier(const SimpleTypeSpecifiers& specs);

// The Table 10 combination, or a throw for combinations the table does
// not contain (including the empty sequence).
EFundamentalType CombineSimpleTypeSpecifiers(
	const SimpleTypeSpecifiers& specs);
