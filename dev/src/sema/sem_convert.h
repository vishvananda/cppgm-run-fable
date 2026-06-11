#pragma once

#include <vector>

using std::vector;

#include "sema/sem_node.h"
#include "sema/type.h"

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
	CR_ELLIPSIS     // matched a trailing ... (13.3.3.1.3)
};

// The conversion-relevant facts of one argument or initializer.
struct ConversionSource
{
	ConversionSource()
		: category(VC_PRVALUE), null_pointer_literal(false),
		  function_set(false)
	{}

	TypePtr type;           // reference-stripped expression type
	EValueCategory category;
	// A literal of integer type with value zero (4.10p1 subset).
	bool null_pointer_literal;
	// An id-expression naming an overloaded function; `overloads` holds
	// the candidate function types (13.4 target-directed resolution).
	bool function_set;
	vector<TypePtr> overloads;
};

// One classified implicit conversion sequence.
struct ImplicitConversion
{
	ImplicitConversion()
		: viable(false), rank(CR_EXACT), null_to_pointer(false),
		  bool_from_pointer(false), reference_binding(false),
		  binds_rvalue_reference(false), base_distance(0),
		  selected_overload(-1)
	{}

	bool viable;
	EConversionRank rank;
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
};

// Classifies the conversion of `source` to the destination type (a
// parameter, variable, or return type; references bind). Returns a
// non-viable result rather than throwing.
ImplicitConversion ClassifyConversion(const ConversionSource& source,
                                      const TypePtr& dest);

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
size_t SelectBestOverload(const vector<TypePtr>& candidates,
                          const vector<ConversionSource>& args,
                          vector<ImplicitConversion>& conversions,
                          const vector<size_t>* min_arity = 0);
