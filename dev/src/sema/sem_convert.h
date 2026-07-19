#pragma once

#include <stdexcept>
#include <vector>

using std::vector;

#include "sema/sem_node.h"
#include "sema/type.h"

// Overload resolution found no viable candidate (13.3.2). Typed so
// recovery paths (the built-in operator form, ADL retry) fall back
// only on genuine no-match — an ambiguous best (13.3.3p2) or a
// selected-but-unusable candidate still propagates as an error.
struct NoViableOverloadError : std::runtime_error
{
	explicit NoViableOverloadError(const string& what)
		: std::runtime_error(what)
	{}
};

// The PA12 standard-conversion subset (clause 4 plus the basic 8.5.3
// reference bindings) and its 13.3 ranking. The classification works
// over (type, value category, null-pointer-constant) facts so the
// expression analyzer, copy-initialization, and overload resolution
// share one rule set.

enum EConversionRank
{
	CR_EXACT,       // identity, lvalue-to-rvalue, decay, qualification
	CR_PROMOTION,   // 4.5 / 4.6
	CR_CONVERSION,  // 4.7-4.12 subset
	CR_USER,        // 13.3.3.1.2 converting-constructor sequence
	CR_ELLIPSIS     // matched a trailing ... (13.3.3.1.3)
};

// The conversion-relevant facts of one argument or initializer.
struct ConversionSource
{
	ConversionSource()
		: category(VC_PRVALUE), null_pointer_literal(false),
		  function_set(false), braced(false)
	{}

	TypePtr type;           // reference-stripped expression type
	EValueCategory category;
	// A literal of integer type with value zero (4.10p1 subset).
	bool null_pointer_literal;
	// An id-expression naming an overloaded function; `overloads` holds
	// the candidate function types (13.4 target-directed resolution).
	bool function_set;
	vector<TypePtr> overloads;
	// PA26: the class of a member-function set (`&C::f`); a member
	// pointer destination selects against it (13.4 with 5.3.1p4).
	const NamedTypeInfo* member_class = 0;
	// A braced-init-list argument (13.3.3.1.5): `type` is null and
	// `list_items` holds the analyzed element facts.
	bool braced;
	vector<ConversionSource> list_items;
};

// One classified implicit conversion sequence.
struct ImplicitConversion
{
	ImplicitConversion()
		: viable(false), rank(CR_EXACT), qualification(false),
		  base_distance(0), null_to_pointer(false),
		  bool_from_pointer(false), reference_binding(false),
		  binds_rvalue_reference(false), selected_overload(-1),
		  user_class(0), user_ctor(-1), conv_class(0), conv_index(-1),
		  second_rank(CR_EXACT)
	{}

	bool viable;
	EConversionRank rank;
	// PA25: the destination is a std::initializer_list specialization
	// (the applied conversion materializes the backing array).
	bool init_list_dest = false;
	// 13.3.3.2p3: an exact-rank sequence with a real qualification
	// conversion loses to the identity form; between two qualification
	// conversions the destination whose cv-signature is a proper
	// subset wins (`qual_dest` records the destination compared).
	bool qualification;
	TypePtr qual_dest;
	// 13.3.3.2p4 derivation-distance tie-break: 0 for non-hierarchy
	// conversions, the base-chain distance for derived-to-base pointer
	// and reference forms, large for conversions to void pointers.
	int base_distance;
	// The source is a null pointer literal converting to a pointer or
	// nullptr_t destination: the dump retypes the literal in place.
	bool null_to_pointer;
	// 13.3.3.2p4: pointer (or member pointer) to bool loses ties.
	bool bool_from_pointer;
	// 13.3.3.2p3 reference-binding tie-breaks.
	bool reference_binding;
	bool binds_rvalue_reference;
	TypePtr referee;  // referenced type of a reference binding
	// Function-set sources: index of the overload the target selected.
	int selected_overload;
	// CR_USER: the destination class and its selected converting
	// constructor (12.3.1).
	const NamedTypeInfo* user_class;
	int user_ctor;
	// PA34 8.5.1: a braced list initializing an aggregate destination
	// field-wise (no constructor is selected; the apply stage builds
	// the field-wise temporary, designated elements included).
	bool aggregate_list = false;
	// CR_USER through a conversion function (12.3.2): the declaring
	// class and its conversion-set index, plus the rank of the second
	// standard conversion (13.3.3.2p3 ranking between user sequences).
	const NamedTypeInfo* conv_class;
	int conv_index;
	EConversionRank second_rank;
	// PA24 5.1.2p6: a captureless closure converting to its function
	// pointer (applied as the function's address, no call emitted).
	bool closure_to_pointer = false;
};

// Classifies the conversion of `source` to the destination type (a
// parameter, variable, or return type; references bind). Returns a
// non-viable result rather than throwing. `contextual` additionally
// admits explicit conversion functions (direct-initialization and
// contextual-bool contexts, 12.3.2p2).
ImplicitConversion ClassifyConversion(const ConversionSource& source,
                                      const TypePtr& dest);
ImplicitConversion ClassifyConversionEx(const ConversionSource& source,
                                        const TypePtr& dest,
                                        bool contextual);

// 4.5 integral promotion target of an integral fundamental type.
EFundamentalType PromotedFundamental(EFundamentalType type);

// 5p9 usual arithmetic conversions over promoted operand types; throws
// outside the arithmetic subset.
TypePtr UsualArithmeticConversions(const TypePtr& a, const TypePtr& b);

// Promotes arithmetic types and unscoped enumerations to their 4.5/4.6
// result; other types are returned unchanged.
TypePtr PromoteForArithmetic(const TypePtr& type);

bool IsUnscopedEnum(const TypePtr& type);
bool IsObjectPointer(const TypePtr& type);

// 13.3.3 best viable function over the candidate function types.
// `arity_exact` candidates must take exactly args.size() parameters
// unless variadic. Throws when no candidate is viable or the best is
// ambiguous; returns the index of the winner and fills the winning
// per-argument conversions. `min_arity`, when given, holds each
// candidate's minimum argument count (parameters minus trailing
// default arguments, 8.3.6).
// PA18 `is_template`, when given, marks candidates that are deduced
// function-template specializations: a non-template candidate beats a
// template specialization when their conversion sequences tie
// (13.3.3p1).
// 13.3.3p1 final template tie-break hook: partial ordering between
// two viable deduced template specializations (14.5.6.2 subset,
// implemented by the binder; the ranking itself stays conversion-only).
struct OverloadOrder
{
	virtual bool MoreSpecialized(size_t a, size_t b) const = 0;
protected:
	~OverloadOrder() {}
};

size_t SelectBestOverload(const vector<TypePtr>& candidates,
                          const vector<ConversionSource>& args,
                          vector<ImplicitConversion>& conversions,
                          const vector<size_t>* min_arity = 0,
                          const vector<bool>* is_template = 0,
                          const OverloadOrder* order = 0);

// PA22 no-eager-instantiation: classifying a conversion to a class
// destination is a completeness demand (14.7.1p4), but the
// classification has no binder dependency. The active binder
// registers its instantiation entry point for the classification to
// call on dormant specializations (null clears it).
void SetConversionCompletionHook(void (*hook)(void* context,
                                              const NamedTypeInfo* info),
                                 void* context);

// PA22 conversion-function templates (14.8.2.3): before a class
// source's conversion functions are consulted, the binder deduces its
// conversion templates against the destination, synthesizing ordinary
// ClassConversion entries for the classification loop to rank.
void SetConversionTemplateHook(void (*hook)(void* context,
                                            const NamedTypeInfo* from,
                                            const TypePtr& dest),
                               void* context);

// PA22 constructor templates in implicit conversions: before a class
// destination's converting constructors are consulted, the binder
// deduces its constructor templates against the source, synthesizing
// ordinary ClassCtor entries for the classification loop to rank.
void SetCtorTemplateHook(void (*hook)(void* context,
                                      const NamedTypeInfo* dest,
                                      const ConversionSource& source),
                         void* context);

// PA24 captureless closures: the classification asks the binder for a
// closure class's function type (null for ordinary classes); a match
// against a pointer-to-function destination converts like the
// function-to-pointer standard conversion.
void SetClosureFunctionHook(TypePtr (*hook)(void* context,
                                            const NamedTypeInfo* cls),
                            void* context);
