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
	TK_TYPE_PARAM,
	// PA12: pointer to member of the class `named` of type `target`
	// (8.3.3). Node cv is the pointer's own qualification.
	TK_MEMBER_POINTER,
	// PA18: a dependent class-template specialization pattern
	// (`box<T>` with T still a template parameter): `named` is the
	// template's anchor entity and `parameters` holds the argument
	// pattern types. Never appears in a concrete (instantiated)
	// declaration; deduction and mangling walk it.
	TK_TEMPLATE_SPEC
};

struct Scope;
struct Type;
typedef shared_ptr<const Type> TypePtr;
struct AstExpr;

// PA19: one template argument - a type, or an integral constant value
// of a declared parameter type. The value stores its (possibly enum-
// underlying) fundamental type and 64-bit two's-complement bits
// directly (the binder-side ConstValue lives in sema/scope.h; this
// header stays below it). Pattern (dependent) arguments appear only
// inside template patterns: `value_param` names the enclosing
// template's value parameter the argument forwards positionally
// (`store<N, U>`), and `dependent_value` carries an argument
// expression whose value is unknown until instantiation. Concrete
// specialization identities never carry either.
struct TemplateArg
{
	TemplateArg()
		: is_value(false), value_type(FT_INT), value_bits(0),
		  value_param(-1), dependent_value(0)
	{}
	explicit TemplateArg(const TypePtr& type_in)
		: type(type_in), is_value(false), value_type(FT_INT),
		  value_bits(0), value_param(-1), dependent_value(0)
	{}

	TypePtr type;  // the type argument; for values, the declared
	               // parameter type (enum identity preserved)
	bool is_value;
	EFundamentalType value_type;
	unsigned long long value_bits;
	int value_param;
	const AstExpr* dependent_value;
	// Pattern slot only: an unexpanded `...` expansion inside a
	// dependent template-id (`tuple<T&&...>` in a pattern); the
	// carried type/expression is the expansion pattern. Instantiation
	// re-resolves the whole template-id concretely.
	bool pack_pattern = false;
	// PA21 template-template argument: the named template entity, or
	// (in a pattern) the enclosing template's template-template
	// parameter slot the argument forwards positionally.
	const struct TemplateInfo* template_entity = 0;
	int template_param = -1;
	// PA21 deduction slots only (never part of a specialization
	// identity): a slot standing for a parameter pack accumulates its
	// deduced element run; `pack_done` is set once one expansion
	// completed the run (later expansions must deduce the same run).
	bool is_pack_slot = false;
	bool pack_done = false;
	vector<TemplateArg> pack_elements;
};

// One record per named-type entity, owned by the semantic model that
// declared it and shared by every Type node naming it, so completing
// the entity (class definition, enum definition) is visible through
// types composed before the completion (3.9p7).
struct NamedTypeInfo
{
	NamedTypeInfo()
		: scope(0), complete(false), is_union(false), is_scoped(false),
		  is_defined(false), enum_underlying(FT_INT), size(0), alignment(0),
		  base_entity(0), class_record(0), spec_template(0),
		  is_template_anchor(false), param_index(-1)
	{}

	string display;  // canonical spelling: "struct C", "enum class FY",
	                 // "typename T", "template-parameter TT"
	// Structural identity: the declaring scope and the bare declared
	// name (the PA14 mangler walks these instead of parsing `display`).
	const Scope* scope;
	string name;
	// PA17: the declared class-key spelling ("struct"/"class"/"union"),
	// recorded at declaration so downstream consumers (the RTTI low
	// names) do not parse `display`. Empty for non-class entities.
	string class_key;
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
	// PA15 single inheritance: the direct base class entity (null when
	// none); the clause 4 conversions walk this chain.
	const NamedTypeInfo* base_entity;
	// PA15: the class metadata record (sema/class_info.h), set when the
	// class opens; the conversion rules read the constructor set.
	const struct ClassInfo* class_record;
	// PA18 template identity. For an instantiated class-template
	// specialization: the source template and the concrete argument
	// list (the mangler emits `NameI<args>E`). For a class template's
	// anchor entity (the identity behind TK_TEMPLATE_SPEC patterns):
	// `is_template_anchor` is set and `spec_args` is empty. For a
	// template type-parameter placeholder used in deduction/mangling
	// patterns: `param_index` is its zero-based position (-1
	// otherwise).
	const struct TemplateInfo* spec_template;
	vector<TemplateArg> spec_args;
	bool is_template_anchor;
	int param_index;
};

// 4.10p3/4.11p2 derivation distance from `from` to `to` along the
// single-inheritance chain: 0 for the same entity, -1 when `to` is not
// a (possibly indirect) base of `from`.
int BaseClassDistance(const NamedTypeInfo* from, const NamedTypeInfo* to);

struct Type
{
	Type()
		: kind(TK_FUNDAMENTAL), is_const(false), is_volatile(false),
		  fundamental(FT_VOID), bound_known(false), bound(0),
		  variadic(false), ref_qual(0), named(0)
	{}

	ETypeKind kind;

	// cv-qualification of this level. Never set on references (8.3.2p1)
	// or arrays (8.3.4p1 stores cv on the element type instead).
	bool is_const;
	bool is_volatile;

	EFundamentalType fundamental;  // TK_FUNDAMENTAL
	TypePtr target;                // pointee / referee / element / return /
	                               // member type (TK_MEMBER_POINTER)
	bool bound_known;              // TK_ARRAY: false for unknown bound
	unsigned long long bound;      // TK_ARRAY when bound_known
	vector<TypePtr> parameters;    // TK_FUNCTION, already adjusted (8.3.5p5)
	bool variadic;                 // TK_FUNCTION
	// TK_FUNCTION: the member-function ref-qualifier (8.3.5p6):
	// 0 none, 1 `&`, 2 `&&`.
	int ref_qual;
	// TK_FUNCTION: member-function cv-qualifiers (8.3.5p6) are stored as
	// the node's is_const/is_volatile and print after the parameter list.
	const NamedTypeInfo* named;    // TK_CLASS / TK_ENUM / TK_TYPE_PARAM /
	                               // TK_MEMBER_POINTER (the class)
	// TK_TEMPLATE_SPEC: the argument pattern list (types and values);
	// `parameters` stays function-only.
	vector<TemplateArg> targs;
	// PA21 pattern-only marker: this function-parameter entry of an
	// abstract TK_FUNCTION pattern was spelled `P...` (deduction
	// absorbs an argument run). Never set on concrete types.
	bool pack_expansion = false;
	// PA21 pattern-only: a TK_ARRAY bound spelled as a value template
	// parameter (`T[N]`); deduction binds the slot from the argument's
	// concrete bound. -1 on concrete types.
	int bound_param = -1;
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

// PA16: the function type with a member ref-qualifier (8.3.5p6).
TypePtr MakeRefQualifiedType(const TypePtr& function, int ref_qual);

// 8.3.3: pointer to member of `cls` of type `member`. Throws on
// reference or void member types (8.3.3p3).
TypePtr MakeMemberPointerType(const NamedTypeInfo* cls,
                              const TypePtr& member, bool is_const,
                              bool is_volatile);

// cv applied through a typedef-name or specifier-seq: redistributes
// onto array element types and is silently dropped on references.
TypePtr MakeFunctionCvQualifiedType(const TypePtr& type, bool add_const,
                                    bool add_volatile);
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

// PA18: a dependent specialization pattern type over the template
// anchored by `anchor` with the given argument pattern.
TypePtr MakeTemplateSpecType(const NamedTypeInfo* anchor,
                             const vector<TemplateArg>& args);

// PA19 template-argument helpers: structural equality (14.4: same
// type, or same declared type and value) and dependence (type
// dependence, a value-parameter slot, or an uninstantiated value
// expression).
bool TemplateArgEquals(const TemplateArg& a, const TemplateArg& b);
bool TemplateArgIsDependent(const TemplateArg& arg);

// PA18: whether any component of `type` is (or contains) a template
// parameter or dependent specialization pattern.
bool TypeIsDependent(const TypePtr& type);

// Structural equality, including cv at every level.
bool TypeEquals(const TypePtr& a, const TypePtr& b);

// The type without its top-level cv-qualifiers; for arrays the
// element's cv is removed instead (3.9.3p5).
TypePtr RemoveTopCv(const TypePtr& type);

// Top-level cv (for arrays: the element's, recursively).
void TopCv(const TypePtr& type, bool& is_const, bool& is_volatile);

// 4.4: pointer to cv1 T -> pointer to cv2 T conversion validity.
bool QualificationConvertible(const TypePtr& from, const TypePtr& to);

// 13.3.3.2p3: `a`'s cv-signature is a proper subset of `b`'s (both
// similar pointer types).
bool CvSignatureProperSubset(const TypePtr& a, const TypePtr& b);

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
