#include "sema/sem_convert.h"

#include <stdexcept>

#include "sema/class_info.h"

using std::runtime_error;

namespace {

// The active binder's constructor-template deduction entry point
// (thread-local, like the completion hook below).
thread_local void (*ctor_template_hook)(void*, const NamedTypeInfo*,
                                        const ConversionSource&) = 0;
thread_local void* ctor_template_context = 0;
// PA24: the binder's captureless-closure function-type query.
thread_local TypePtr (*closure_function_hook)(void*,
                                              const NamedTypeInfo*) = 0;
thread_local void* closure_function_context = 0;

bool IsBoolType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL && type->fundamental == FT_BOOL;
}

// 4.1/4.2/4.3 value transformation of a glvalue/array/function source:
// arrays and functions decay to pointers, everything else drops its
// top-level cv. All three carry Exact rank.
TypePtr DecayedValueType(const TypePtr& type)
{
	if (type->kind == TK_ARRAY)
		return MakePointerType(type->target, false, false);
	if (type->kind == TK_FUNCTION)
		return MakePointerType(type, false, false);
	return RemoveTopCv(type);
}

bool CvSuperset(const TypePtr& more, const TypePtr& less)
{
	return (more->is_const || !less->is_const) &&
		(more->is_volatile || !less->is_volatile);
}

// 8.5.3p4: cv1 T1 reference-related to cv2 T2 (same type ignoring top
// cv); reference-compatible additionally requires cv1 >= cv2.
bool ReferenceRelated(const TypePtr& referee, const TypePtr& source)
{
	return TypeEquals(RemoveTopCv(referee), RemoveTopCv(source));
}

bool ReferenceCompatible(const TypePtr& referee, const TypePtr& source)
{
	bool referee_const = false;
	bool referee_volatile = false;
	bool source_const = false;
	bool source_volatile = false;
	TopCv(referee, referee_const, referee_volatile);
	TopCv(source, source_const, source_volatile);
	return ReferenceRelated(referee, source) &&
		(referee_const || !source_const) &&
		(referee_volatile || !source_volatile);
}

// Target-directed resolution of an overloaded function name against a
// required function type (13.4): exactly one overload must match.
bool SelectFunctionFromSet(const vector<TypePtr>& overloads,
                           const TypePtr& function_type,
                           ImplicitConversion& result)
{
	int found = -1;
	for (size_t i = 0; i < overloads.size(); i++)
	{
		if (!TypeEquals(overloads[i], function_type))
			continue;
		if (found >= 0)
			return false;
		found = (int)i;
	}
	if (found < 0)
		return false;
	result.viable = true;
	result.rank = CR_EXACT;
	result.selected_overload = found;
	return true;
}

ImplicitConversion ClassifyFunctionSet(const ConversionSource& source,
                                       const TypePtr& dest)
{
	ImplicitConversion result;
	if (dest->kind == TK_POINTER && dest->target->kind == TK_FUNCTION)
		SelectFunctionFromSet(source.overloads, dest->target, result);
	// PA26 13.4: a member-function set resolves against a member
	// pointer destination when the classes agree.
	if (dest->kind == TK_MEMBER_POINTER &&
	    dest->target->kind == TK_FUNCTION && source.member_class &&
	    BaseClassDistance(dest->named, source.member_class) >= 0)
		SelectFunctionFromSet(source.overloads, dest->target, result);
	return result;
}

// `allow_user` controls whether user-defined conversions (converting
// constructors, conversion functions) participate. A user conversion's
// own nested classifications run with it false — 13.3.3.1.2p1 permits
// one user conversion per sequence — which also bounds the recursion
// over mutually convertible classes.
ImplicitConversion ClassifyReferenceBinding(const ConversionSource& source,
                                            const TypePtr& dest,
                                            bool allow_user);
ImplicitConversion ClassifySourceConversionFunction(
	const ConversionSource& source, const TypePtr& dest, bool contextual);
ImplicitConversion ClassifyConversionImpl(const ConversionSource& source,
                                          const TypePtr& dest,
                                          bool contextual, bool allow_user);
ImplicitConversion ClassifyListInitSequence(const ConversionSource& source,
                                            const TypePtr& dest,
                                            bool allow_user);

// 4.5/4.6/4.7-4.12 over non-reference destinations. The destination is
// taken as an object value: its own top-level cv is ignored.
ImplicitConversion ClassifyValueConversion(const ConversionSource& source,
                                           const TypePtr& dest_in,
                                           bool allow_user)
{
	ImplicitConversion result;
	TypePtr dest = RemoveTopCv(dest_in);
	if (dest->kind == TK_ARRAY)
		return result;  // only braced/string forms initialize arrays
	if (source.function_set)
		return ClassifyFunctionSet(source, dest);

	TypePtr from = DecayedValueType(source.type);
	// PA24 5.1.2p6: a captureless closure converts to the pointer to
	// its function like the function-to-pointer conversion (Exact
	// rank, no conversion-function call).
	if (dest->kind == TK_POINTER && dest->target->kind == TK_FUNCTION &&
	    from->kind == TK_CLASS && closure_function_hook)
	{
		TypePtr fn = closure_function_hook(closure_function_context,
		                                   from->named);
		if (fn && TypeEquals(RemoveTopCv(fn), RemoveTopCv(dest->target)))
		{
			result.viable = true;
			result.rank = CR_EXACT;
			result.closure_to_pointer = true;
			return result;
		}
	}
	if (TypeEquals(from, dest))
	{
		result.viable = true;
		result.rank = CR_EXACT;
		return result;
	}
	if (IsArithmeticType(dest))
	{
		if (IsArithmeticType(from) || IsUnscopedEnum(from))
		{
			result.viable = true;
			TypePtr promoted = PromoteForArithmetic(from);
			result.rank = TypeEquals(promoted, dest)
				? CR_PROMOTION : CR_CONVERSION;
			return result;
		}
		if (IsBoolType(dest) &&
		    (from->kind == TK_POINTER || from->kind == TK_MEMBER_POINTER ||
		     IsNullPtrType(from)))
		{
			// 4.12 boolean conversions; the pointer forms lose the
			// 13.3.3.2p4 tie-break.
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.bool_from_pointer = !IsNullPtrType(from);
			return result;
		}
		return result;
	}
	if (IsNullPtrType(dest))
	{
		// 4.10p1: a null pointer constant converts to nullptr_t.
		if (source.null_pointer_literal)
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.null_to_pointer = true;
		}
		return result;
	}
	if (dest->kind == TK_POINTER)
	{
		if (source.null_pointer_literal || IsNullPtrType(from))
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.null_to_pointer = source.null_pointer_literal;
			return result;
		}
		if (from->kind != TK_POINTER)
			return result;
		if (QualificationConvertible(from, dest))
		{
			result.viable = true;
			result.rank = CR_EXACT;
			// 13.3.3.2p3: identity beats a real qualification step.
			result.qualification = !TypeEquals(from, dest);
			if (result.qualification)
				result.qual_dest = dest;
			return result;
		}
		// 4.10p3: pointer to derived converts to pointer to a (no less
		// qualified) base; nearer bases rank better (13.3.3.2p4).
		// PA27: shared virtual bases participate (the lowering rides
		// the carrier entry).
		if (from->target->kind == TK_CLASS &&
		    dest->target->kind == TK_CLASS &&
		    CvSuperset(dest->target, from->target))
		{
			int distance = BaseClassDistance(from->target->named,
			                                 dest->target->named);
			if (distance <= 0 &&
			    from->target->named != dest->target->named &&
			    from->target->named->class_record)
			{
				size_t vbase_index = 0;
				unsigned long long remainder = 0;
				if (VirtualBasePath(*from->target->named->class_record,
				                    dest->target->named, vbase_index,
				                    remainder))
					distance = 1;
			}
			if (distance > 0)
			{
				result.viable = true;
				result.rank = CR_CONVERSION;
				result.base_distance = distance;
				return result;
			}
		}
		// 4.10p2: object pointer to (cv-compatible) void pointer.
		if (IsVoidType(RemoveTopCv(dest->target)) &&
		    from->target->kind != TK_FUNCTION &&
		    !IsVoidType(RemoveTopCv(from->target)) &&
		    CvSuperset(dest->target, from->target))
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.base_distance = 1 << 20;
		}
		return result;
	}
	if (dest->kind == TK_MEMBER_POINTER &&
	    (source.null_pointer_literal || IsNullPtrType(from)))
	{
		result.viable = true;
		result.rank = CR_CONVERSION;
		result.null_to_pointer = source.null_pointer_literal;
		return result;
	}
	if (dest->kind == TK_MEMBER_POINTER &&
	    from->kind == TK_MEMBER_POINTER)
	{
		// 4.4p2: a qualification conversion applies over the member
		// type (member pointers act as one more pointer level).
		bool qualification = false;
		if (!TypeEquals(from->target, dest->target))
		{
			if (!QualificationConvertible(
			        MakePointerType(from->target, false, false),
			        MakePointerType(dest->target, false, false)))
				return result;
			qualification = true;
		}
		if (from->named == dest->named)
		{
			result.viable = true;
			result.rank = CR_EXACT;
			result.qualification = qualification;
			if (qualification)
				result.qual_dest = dest;
			return result;
		}
		// 4.11p2: pm of base converts to pm of (unambiguous) derived.
		int distance = BaseClassDistance(dest->named, from->named);
		if (distance > 0)
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.base_distance = distance;
		}
		return result;
	}
	// 13.3.3.1: a derived-class value copy-initializes a base-class
	// object directly with Conversion rank (the base copy constructor
	// binds the derived source).
	if (dest->kind == TK_CLASS && from->kind == TK_CLASS)
	{
		int distance = BaseClassDistance(from->named, dest->named);
		if (distance > 0)
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.base_distance = distance;
			return result;
		}
	}
	// 13.3.3.1.2/12.3.1: a class destination accepts sources its
	// non-explicit converting constructors take through one standard
	// conversion (the PA15 user-defined-conversion subset). PA22:
	// constructor templates deduce against the source first,
	// synthesizing their entries for the same loop.
	if (allow_user && dest->kind == TK_CLASS && ctor_template_hook &&
	    !(from->kind == TK_CLASS &&
	      BaseClassDistance(from->named, dest->named) >= 0))
		ctor_template_hook(ctor_template_context, dest->named, source);
	if (allow_user && dest->kind == TK_CLASS && dest->named->class_record &&
	    !(from->kind == TK_CLASS &&
	      BaseClassDistance(from->named, dest->named) >= 0))
	{
		const ClassInfo& cls = *dest->named->class_record;
		for (size_t i = 0; i < cls.ctors.size(); i++)
		{
			const ClassCtor& ctor = cls.ctors[i];
			if (ctor.is_explicit || ctor.deleted ||
			    ctor.kind != CK_ORDINARY ||
			    ctor.type->parameters.size() != 1)
				continue;
			const TypePtr& param = ctor.type->parameters[0];
			// One *standard* conversion reaches the parameter: the
			// nested classification runs without user conversions.
			ImplicitConversion inner =
				ClassifyConversionImpl(source, param, false, false);
			if (!inner.viable)
				continue;
			result.viable = true;
			result.rank = CR_USER;
			result.user_class = dest->named;
			result.user_ctor = (int)i;
			return result;
		}
	}
	return result;
}

ImplicitConversion ClassifyReferenceBinding(const ConversionSource& source,
                                            const TypePtr& dest,
                                            bool allow_user)
{
	ImplicitConversion result;
	const TypePtr& referee = dest->target;
	bool rvalue_ref = dest->kind == TK_RVALUE_REFERENCE;
	result.reference_binding = true;
	result.binds_rvalue_reference = rvalue_ref;
	result.referee = referee;

	if (source.function_set)
	{
		if (referee->kind == TK_FUNCTION)
		{
			SelectFunctionFromSet(source.overloads, referee, result);
			return result;
		}
		// 8.5.3p5: the function-to-pointer converted temporary binds a
		// const lvalue (or rvalue) reference to pointer-to-function.
		bool set_referee_const = false;
		bool set_referee_volatile = false;
		TopCv(referee, set_referee_const, set_referee_volatile);
		bool set_const_ref = !rvalue_ref && set_referee_const &&
			!set_referee_volatile;
		if ((rvalue_ref || set_const_ref) &&
		    RemoveTopCv(referee)->kind == TK_POINTER &&
		    RemoveTopCv(referee)->target->kind == TK_FUNCTION)
		{
			ImplicitConversion value = ClassifyValueConversion(
				source, RemoveTopCv(referee), allow_user);
			if (value.viable)
			{
				result.viable = true;
				result.rank = value.rank;
				result.selected_overload = value.selected_overload;
			}
		}
		return result;
	}

	bool function_source = source.type->kind == TK_FUNCTION;
	bool lvalue_source = source.category == VC_LVALUE;
	if (rvalue_ref)
	{
		// 8.5.3p5: an rvalue reference never binds an lvalue directly;
		// a conversion-produced temporary (array decay, value
		// conversions) still binds below.
		if (lvalue_source && !function_source &&
		    ReferenceRelated(referee, source.type))
			return result;
	}
	else if (!lvalue_source)
	{
		// An lvalue reference takes rvalues only when const (and not
		// volatile), binding a temporary (8.5.3p5).
		bool referee_const = false;
		bool referee_volatile = false;
		TopCv(referee, referee_const, referee_volatile);
		if (!referee_const || referee_volatile)
			return result;
	}
	if (ReferenceCompatible(referee, source.type))
	{
		result.viable = true;
		result.rank = CR_EXACT;
		return result;
	}
	if (ReferenceRelated(referee, source.type))
		return result;  // related but lesser-qualified: ill-formed
	// 8.5.3p4 with 13.3.3.1.4p1: a base-class reference binds a derived
	// object directly with Conversion rank.
	if (referee->kind == TK_CLASS && source.type->kind == TK_CLASS &&
	    CvSuperset(referee, source.type))
	{
		int distance = BaseClassDistance(source.type->named,
		                                 referee->named);
		if (distance > 0)
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.base_distance = distance;
			return result;
		}
		// PA21: an extra (empty) base binds at the object's address.
		if (distance < 0 &&
		    DerivedFromWithExtrasLinked(source.type->named,
		                                referee->named))
		{
			result.viable = true;
			result.rank = CR_CONVERSION;
			result.base_distance = 0;
			return result;
		}
	}
	// Not reference-related: a const lvalue reference (or an rvalue
	// reference over an rvalue) binds a temporary created by the value
	// conversion, which carries the conversion's rank (13.3.3.1.4p2).
	bool referee_const = false;
	bool referee_volatile = false;
	TopCv(referee, referee_const, referee_volatile);
	bool const_lvalue_ref = !rvalue_ref && referee_const && !referee_volatile;
	if (!const_lvalue_ref && !rvalue_ref)
		return result;
	// 8.5.3p5: a conversion function of the source class yielding a
	// compatible referee binds directly and is preferred over a
	// constructor-built temporary.
	if (allow_user && RemoveTopCv(source.type)->kind == TK_CLASS)
	{
		ImplicitConversion via_fn =
			ClassifySourceConversionFunction(source, dest, false);
		if (via_fn.viable)
		{
			via_fn.reference_binding = true;
			via_fn.binds_rvalue_reference = rvalue_ref;
			via_fn.referee = referee;
			return via_fn;
		}
	}
	ImplicitConversion value =
		ClassifyValueConversion(source, RemoveTopCv(referee), allow_user);
	if (!value.viable)
		return result;
	result.viable = true;
	result.rank = value.rank;
	result.null_to_pointer = value.null_to_pointer;
	result.bool_from_pointer = value.bool_from_pointer;
	result.selected_overload = value.selected_overload;
	result.user_class = value.user_class;
	result.user_ctor = value.user_ctor;
	return result;
}

// -1 when a is the better conversion for this source, 1 when b is, 0
// when neither (13.3.3.2).
int CompareConversions(const ImplicitConversion& a,
                       const ImplicitConversion& b,
                       const ConversionSource& source)
{
	if (a.rank != b.rank)
		return a.rank < b.rank ? -1 : 1;
	if (a.rank == CR_USER && a.second_rank != b.second_rank)
		// 13.3.3.2p3: user-defined sequences rank by their second
		// standard conversion.
		return a.second_rank < b.second_rank ? -1 : 1;
	if (a.bool_from_pointer != b.bool_from_pointer)
		return a.bool_from_pointer ? 1 : -1;
	if (a.qualification != b.qualification)
		// 13.3.3.2p3: the sequence without the qualification
		// conversion is the proper subsequence.
		return a.qualification ? 1 : -1;
	if (a.qualification && a.qual_dest && b.qual_dest &&
	    !TypeEquals(a.qual_dest, b.qual_dest))
	{
		// 13.3.3.2p3: between two qualification conversions of the same
		// source, the destination whose cv-signature is a proper subset
		// of the other's is the better sequence.
		if (CvSignatureProperSubset(a.qual_dest, b.qual_dest))
			return -1;
		if (CvSignatureProperSubset(b.qual_dest, a.qual_dest))
			return 1;
	}
	if (a.base_distance != b.base_distance)
		return a.base_distance < b.base_distance ? -1 : 1;
	if (!a.reference_binding || !b.reference_binding)
		return 0;
	if (a.binds_rvalue_reference != b.binds_rvalue_reference)
	{
		// p3: rvalue references prefer rvalues; function lvalues prefer
		// lvalue references.
		bool function_lvalue = source.category == VC_LVALUE &&
			source.type->kind == TK_FUNCTION;
		bool rvalue_better = !function_lvalue;
		if (a.binds_rvalue_reference)
			return rvalue_better ? -1 : 1;
		return rvalue_better ? 1 : -1;
	}
	if (a.referee && b.referee && ReferenceRelated(a.referee, b.referee) &&
	    !TypeEquals(a.referee, b.referee))
	{
		// p3: the less cv-qualified referee wins (an array's cv lives
		// on its element type, 3.9.3p2).
		TypePtr ra = a.referee;
		TypePtr rb = b.referee;
		while (ra->kind == TK_ARRAY)
			ra = ra->target;
		while (rb->kind == TK_ARRAY)
			rb = rb->target;
		bool b_super = CvSuperset(rb, ra);
		bool a_super = CvSuperset(ra, rb);
		if (b_super && !a_super)
			return -1;
		if (a_super && !b_super)
			return 1;
	}
	return 0;
}

struct ViableCandidate
{
	size_t index;
	vector<ImplicitConversion> conversions;
};

// True when `a` is a better viable function than `b` (13.3.3p1).
bool BetterCandidate(const ViableCandidate& a, const ViableCandidate& b,
                     const vector<ConversionSource>& args)
{
	bool better_somewhere = false;
	for (size_t i = 0; i < args.size(); i++)
	{
		int order = CompareConversions(a.conversions[i], b.conversions[i],
		                               args[i]);
		if (order > 0)
			return false;
		if (order < 0)
			better_somewhere = true;
	}
	return better_somewhere;
}

}  // namespace

bool IsUnscopedEnum(const TypePtr& type)
{
	return type->kind == TK_ENUM && !type->named->is_scoped;
}

bool IsObjectPointer(const TypePtr& type)
{
	return type->kind == TK_POINTER &&
		type->target->kind != TK_FUNCTION &&
		!IsVoidType(RemoveTopCv(type->target));
}

EFundamentalType PromotedFundamental(EFundamentalType type)
{
	switch (type)
	{
	case FT_BOOL:
	case FT_CHAR:
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:  // int represents all values (2 bytes)
	case FT_WCHAR_T:   // int on the x86-64 ABI
		return FT_INT;
	case FT_CHAR32_T:  // 4 unsigned bytes: unsigned int (4.5p2)
		return FT_UNSIGNED_INT;
	default:
		return type;
	}
}

TypePtr PromoteForArithmetic(const TypePtr& type)
{
	if (IsUnscopedEnum(type))
		// The PA11 model records the (fixed or int) underlying type;
		// 4.5p4 promotes from it.
		return MakeFundamentalType(
			PromotedFundamental(type->named->enum_underlying));
	if (IsIntegralType(type))
		return MakeFundamentalType(PromotedFundamental(type->fundamental));
	if (IsArithmeticType(type))
		return RemoveTopCv(type);  // floating: unary contexts keep float
	return type;
}

namespace {

// {int, long, long long} conversion rank of a promoted integral type.
int IntegerRank(EFundamentalType type)
{
	switch (type)
	{
	case FT_INT:
	case FT_UNSIGNED_INT:
		return 1;
	case FT_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
		return 2;
	default:
		return 3;
	}
}

bool IsUnsignedFundamental(EFundamentalType type)
{
	return IsIntegralFundamental(type) &&
		!IsSignedIntegralFundamental(type);
}

EFundamentalType UnsignedCounterpart(EFundamentalType type)
{
	switch (type)
	{
	case FT_INT: return FT_UNSIGNED_INT;
	case FT_LONG_INT: return FT_UNSIGNED_LONG_INT;
	default: return FT_UNSIGNED_LONG_LONG_INT;
	}
}

}  // namespace

TypePtr UsualArithmeticConversions(const TypePtr& a_in, const TypePtr& b_in)
{
	TypePtr a = PromoteForArithmetic(a_in);
	TypePtr b = PromoteForArithmetic(b_in);
	if (!IsArithmeticType(a) || !IsArithmeticType(b))
		throw runtime_error("operands are outside the arithmetic subset");
	EFundamentalType fa = a->fundamental;
	EFundamentalType fb = b->fundamental;
	if (fa == FT_LONG_DOUBLE || fb == FT_LONG_DOUBLE)
		return MakeFundamentalType(FT_LONG_DOUBLE);
	if (fa == FT_DOUBLE || fb == FT_DOUBLE)
		return MakeFundamentalType(FT_DOUBLE);
	if (fa == FT_FLOAT || fb == FT_FLOAT)
		return MakeFundamentalType(FT_FLOAT);
	fa = PromotedFundamental(fa);
	fb = PromotedFundamental(fb);
	if (fa == fb)
		return MakeFundamentalType(fa);
	bool ua = IsUnsignedFundamental(fa);
	bool ub = IsUnsignedFundamental(fb);
	if (ua == ub)
		return MakeFundamentalType(
			IntegerRank(fa) >= IntegerRank(fb) ? fa : fb);
	EFundamentalType us = ua ? fa : fb;
	EFundamentalType ss = ua ? fb : fa;
	if (IntegerRank(us) >= IntegerRank(ss))
		return MakeFundamentalType(us);
	// On LP64 a higher-ranked signed type represents the whole unsigned
	// range only when it is wider: long/long long over unsigned int
	// stay signed; long long over unsigned long goes unsigned (5p9).
	if (IntegerRank(us) == 1)
		return MakeFundamentalType(ss);
	return MakeFundamentalType(UnsignedCounterpart(ss));
}

namespace {

// 12.3.2/13.3.3.1.2: the conversion functions of a class source. The
// implicit object binding selects among cv-qualified overloads; the
// result then reaches `dest` through one standard conversion whose
// rank orders competing user-defined sequences.
// The active binder's conversion-template deduction entry point
// (thread-local, like the completion hook above).
thread_local void (*conversion_template_hook)(void*, const NamedTypeInfo*,
                                              const TypePtr&) = 0;
thread_local void* conversion_template_context = 0;

ImplicitConversion ClassifySourceConversionFunction(
	const ConversionSource& source, const TypePtr& dest, bool contextual)
{
	ImplicitConversion result;
	TypePtr from = RemoveTopCv(source.type);
	if (from->kind != TK_CLASS)
		return result;
	// 14.8.2.3: conversion templates deduce against the destination
	// before the declared conversions are ranked.
	if (conversion_template_hook)
		conversion_template_hook(conversion_template_context,
		                         from->named, dest);
	if (!from->named->class_record)
		return result;
	bool dest_bool = !IsReferenceType(dest) && IsBoolType(RemoveTopCv(dest));
	ImplicitConversion best_object;
	// 13.3.3.1p10: two conversion functions no better than each other
	// make the user-defined sequence ambiguous; the source then has no
	// viable conversion to `dest` (a strictly better later candidate
	// clears the tie). 13.3.3p1: a non-template conversion function
	// beats a deduced specialization with equal sequences.
	bool tie = false;
	bool best_is_template = false;
	vector<const ClassInfo*> subtree;
	CollectClassAndBases(from->named->class_record, subtree);
	for (size_t c = 0; c < subtree.size(); c++)
	{
		const ClassInfo* link = subtree[c];
		for (size_t i = 0; i < link->conversions.size(); i++)
		{
			const ClassConversion& conv = link->conversions[i];
			// 12.3.2p2: explicit conversion functions participate only
			// in direct-initialization / contextual-bool contexts.
			if (conv.is_explicit && !(contextual && dest_bool))
				continue;
			// The implicit object parameter binds the source.
			TypePtr object_class = MakeNamedType(TK_CLASS, from->named);
			object_class = MakeCvQualifiedType(
				object_class, conv.type->is_const,
				conv.type->is_volatile);
			ImplicitConversion object = ClassifyReferenceBinding(
				source, MakeReferenceType(object_class, false, true),
				false);
			if (!object.viable)
				continue;
			// One *standard* conversion from the result to `dest`.
			ConversionSource inner;
			inner.type = IsReferenceType(conv.result)
				? conv.result->target : RemoveTopCv(conv.result);
			inner.category = conv.result->kind == TK_LVALUE_REFERENCE
				? VC_LVALUE : VC_PRVALUE;
			ImplicitConversion second =
				ClassifyConversionImpl(inner, dest, false, false);
			if (!second.viable)
				continue;
			bool better = false;
			bool cand_is_template = conv.spec != 0;
			if (!result.viable)
				better = true;
			else
			{
				int object_order = CompareConversions(object, best_object,
				                                      source);
				if (object_order < 0)
					better = true;
				else if (object_order == 0 &&
				         second.rank < result.second_rank)
					better = true;
				else if (object_order == 0 &&
				         second.rank == result.second_rank)
				{
					if (!cand_is_template && best_is_template)
						better = true;
					else if (cand_is_template == best_is_template)
						tie = true;
				}
			}
			if (!better)
				continue;
			tie = false;
			result.viable = true;
			result.rank = CR_USER;
			result.conv_class = link->entity;
			result.conv_index = (int)i;
			result.second_rank = second.rank;
			result.null_to_pointer = false;
			best_object = object;
			best_is_template = cand_is_template;
		}
	}
	if (tie)
		return ImplicitConversion();  // ambiguous: no viable sequence
	return result;
}

}  // namespace

namespace {

// The active binder's instantiation entry point (thread-local: each
// translation unit binds on its own worker thread).
thread_local void (*completion_hook)(void*, const NamedTypeInfo*) = 0;
thread_local void* completion_context = 0;

// 14.7.1p4: a class endpoint (or referee) of a conversion that is a dormant
// specialization instantiates before its constructors and conversion
// functions are consulted.
void DemandClassCompleteness(const TypePtr& dest)
{
	if (!completion_hook)
		return;
	TypePtr bare = dest;
	if (IsReferenceType(bare))
		bare = bare->target;
	bare = RemoveTopCv(bare);
	if (bare->kind == TK_CLASS && !bare->named->complete)
		completion_hook(completion_context, bare->named);
}

// 13.3.3.1.5 list-initialization sequences over the PA24 subset: a
// braced argument reaching a reference-to-array parameter builds a
// temporary array; one reaching a class (or reference-to-class)
// parameter list-initializes a temporary through the class's
// non-explicit constructors; a scalar takes its single element.
ImplicitConversion ClassifyListInitClass(const ConversionSource& source,
                                         const TypePtr& dest,
                                         bool allow_user)
{
	ImplicitConversion result;
	// PA25 13.3.3.1.5p2: a std::initializer_list<T> destination
	// converts each element to T; the worst element conversion ranks
	// the sequence.
	TypePtr list_element;
	if (IsStdInitializerList(dest, &list_element))
	{
		DemandClassCompleteness(dest);
		TypePtr element = RemoveTopCv(list_element);
		EConversionRank worst = CR_EXACT;
		for (size_t i = 0; i < source.list_items.size(); i++)
		{
			ImplicitConversion inner = ClassifyConversionImpl(
				source.list_items[i], element, false, allow_user);
			if (!inner.viable)
				return result;
			if (inner.rank > worst)
				worst = inner.rank;
		}
		result.viable = true;
		result.rank = worst;
		result.init_list_dest = true;
		return result;
	}
	if (!allow_user)
		return result;
	DemandClassCompleteness(dest);
	if (ctor_template_hook)
		ctor_template_hook(ctor_template_context, dest->named, source);
	if (!dest->named->class_record)
		return result;
	const ClassInfo& cls = *dest->named->class_record;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		const ClassCtor& ctor = cls.ctors[i];
		if (ctor.is_explicit || ctor.deleted ||
		    ctor.kind != CK_ORDINARY ||
		    ctor.type->parameters.size() != source.list_items.size())
			continue;
		bool viable = true;
		for (size_t j = 0; viable && j < source.list_items.size(); j++)
		{
			ImplicitConversion inner = ClassifyConversionImpl(
				source.list_items[j], ctor.type->parameters[j],
				false, false);
			viable = inner.viable;
		}
		if (!viable)
			continue;
		result.viable = true;
		result.rank = CR_USER;
		result.user_class = dest->named;
		result.user_ctor = (int)i;
		return result;
	}
	return result;
}

ImplicitConversion ClassifyListInitSequence(const ConversionSource& source,
                                            const TypePtr& dest,
                                            bool allow_user)
{
	ImplicitConversion result;
	if (IsReferenceType(dest))
	{
		const TypePtr& referee = dest->target;
		bool rvalue_ref = dest->kind == TK_RVALUE_REFERENCE;
		// The list always materializes a temporary: an lvalue
		// reference binds it only when const (8.5.3p5).
		if (!rvalue_ref)
		{
			TypePtr element = referee;
			while (element->kind == TK_ARRAY)
				element = element->target;
			bool referee_const = false;
			bool referee_volatile = false;
			TopCv(element, referee_const, referee_volatile);
			if (!referee_const || referee_volatile)
				return result;
		}
		if (referee->kind == TK_ARRAY)
		{
			if (!referee->bound_known ||
			    source.list_items.size() > referee->bound)
				return result;
			TypePtr element = RemoveTopCv(referee->target);
			EConversionRank worst = CR_EXACT;
			for (size_t i = 0; i < source.list_items.size(); i++)
			{
				ImplicitConversion inner = ClassifyConversionImpl(
					source.list_items[i], element, false, false);
				if (!inner.viable)
					return result;
				if (inner.rank > worst)
					worst = inner.rank;
			}
			result.viable = true;
			result.rank = worst;
			result.reference_binding = true;
			result.binds_rvalue_reference = rvalue_ref;
			result.referee = referee;
			return result;
		}
		result = RemoveTopCv(referee)->kind == TK_CLASS
			? ClassifyListInitClass(source, RemoveTopCv(referee),
			                        allow_user)
			: ClassifyListInitSequence(source, RemoveTopCv(referee),
			                           allow_user);
		result.reference_binding = true;
		result.binds_rvalue_reference = rvalue_ref;
		result.referee = referee;
		return result;
	}
	TypePtr bare = RemoveTopCv(dest);
	if (bare->kind == TK_CLASS ||
	    (bare->kind == TK_TEMPLATE_SPEC &&
	     IsStdInitializerList(bare, 0)))
		return ClassifyListInitClass(source, bare, allow_user);
	if (bare->kind == TK_ARRAY)
		return result;  // arrays are never parameter values
	// 8.5.4p3: a scalar destination takes its single element (with the
	// element's rank); an empty list value-initializes.
	if (source.list_items.empty())
	{
		result.viable = true;
		result.rank = CR_EXACT;
		return result;
	}
	if (source.list_items.size() > 1)
		return result;
	return ClassifyConversionImpl(source.list_items[0], bare, false,
	                              allow_user);
}

ImplicitConversion ClassifyConversionImpl(const ConversionSource& source,
                                          const TypePtr& dest,
                                          bool contextual, bool allow_user)
{
	if (source.braced)
		return ClassifyListInitSequence(source, dest, allow_user);
	if (allow_user)
	{
		DemandClassCompleteness(dest);
		// A class source consults its conversion functions.
		if (source.type && !source.function_set)
			DemandClassCompleteness(source.type);
	}
	ImplicitConversion result = IsReferenceType(dest)
		? ClassifyReferenceBinding(source, dest, allow_user)
		: ClassifyValueConversion(source, dest, allow_user);
	if (result.viable)
		return result;
	if (allow_user && source.type &&
	    RemoveTopCv(source.type)->kind == TK_CLASS &&
	    !source.function_set)
		return ClassifySourceConversionFunction(source, dest, contextual);
	return result;
}

}  // namespace

void SetConversionCompletionHook(void (*hook)(void* context,
                                              const NamedTypeInfo* info),
                                 void* context)
{
	completion_hook = hook;
	completion_context = context;
}

void SetConversionTemplateHook(void (*hook)(void* context,
                                            const NamedTypeInfo* from,
                                            const TypePtr& dest),
                               void* context)
{
	conversion_template_hook = hook;
	conversion_template_context = context;
}

void SetClosureFunctionHook(TypePtr (*hook)(void* context,
                                            const NamedTypeInfo* cls),
                            void* context)
{
	closure_function_hook = hook;
	closure_function_context = context;
}

void SetCtorTemplateHook(void (*hook)(void* context,
                                      const NamedTypeInfo* dest,
                                      const ConversionSource& source),
                         void* context)
{
	ctor_template_hook = hook;
	ctor_template_context = context;
}

ImplicitConversion ClassifyConversion(const ConversionSource& source,
                                      const TypePtr& dest)
{
	return ClassifyConversionImpl(source, dest, false, true);
}

ImplicitConversion ClassifyConversionEx(const ConversionSource& source,
                                        const TypePtr& dest,
                                        bool contextual)
{
	return ClassifyConversionImpl(source, dest, contextual, true);
}

size_t SelectBestOverload(const vector<TypePtr>& candidates,
                          const vector<ConversionSource>& args,
                          vector<ImplicitConversion>& conversions,
                          const vector<size_t>* min_arity,
                          const vector<bool>* is_template,
                          const OverloadOrder* order)
{
	vector<ViableCandidate> viable;
	for (size_t c = 0; c < candidates.size(); c++)
	{
		const TypePtr& fn = candidates[c];
		const vector<TypePtr>& params = fn->parameters;
		size_t required = min_arity ? (*min_arity)[c] : params.size();
		if (args.size() < required ||
		    (args.size() > params.size() && !fn->variadic))
			continue;
		ViableCandidate candidate;
		candidate.index = c;
		bool all_viable = true;
		for (size_t i = 0; i < args.size(); i++)
		{
			ImplicitConversion conversion;
			if (i < params.size())
				conversion = ClassifyConversion(args[i], params[i]);
			else
			{
				conversion.viable = true;
				conversion.rank = CR_ELLIPSIS;
			}
			if (!conversion.viable)
			{
				all_viable = false;
				break;
			}
			candidate.conversions.push_back(conversion);
		}
		if (all_viable)
			viable.push_back(candidate);
	}
	if (viable.empty())
		throw NoViableOverloadError("no matching function for call");
	// PA18 13.3.3p1 final tie-break: a non-template candidate beats a
	// deduced template specialization with equal conversion sequences.
	struct Better
	{
		const vector<ConversionSource>& args;
		const vector<bool>* is_template;
		const OverloadOrder* order;
		bool operator()(const ViableCandidate& a,
		                const ViableCandidate& b) const
		{
			if (BetterCandidate(a, b, args))
				return true;
			if (BetterCandidate(b, a, args))
				return false;
			if (is_template && !(*is_template)[a.index] &&
			    (*is_template)[b.index])
				return true;
			// 14.5.6.2: both template specializations with equal
			// conversions rank by partial ordering.
			if (order && is_template && (*is_template)[a.index] &&
			    (*is_template)[b.index])
				return order->MoreSpecialized(a.index, b.index);
			return false;
		}
	};
	Better better = {args, is_template, order};
	size_t best = 0;
	for (size_t i = 1; i < viable.size(); i++)
		if (better(viable[i], viable[best]))
			best = i;
	for (size_t i = 0; i < viable.size(); i++)
		if (i != best && !better(viable[best], viable[i]))
			throw runtime_error("ambiguous overloaded call");
	conversions = viable[best].conversions;
	return viable[best].index;
}
