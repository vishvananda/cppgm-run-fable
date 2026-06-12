#include "sema/sem_convert.h"

#include <stdexcept>

#include "sema/class_info.h"

using std::runtime_error;

namespace {

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
	return result;
}

ImplicitConversion ClassifyReferenceBinding(const ConversionSource& source,
                                            const TypePtr& dest);
ImplicitConversion ClassifySourceConversionFunction(
	const ConversionSource& source, const TypePtr& dest, bool contextual);

// 4.5/4.6/4.7-4.12 over non-reference destinations. The destination is
// taken as an object value: its own top-level cv is ignored.
ImplicitConversion ClassifyValueConversion(const ConversionSource& source,
                                           const TypePtr& dest_in)
{
	ImplicitConversion result;
	TypePtr dest = RemoveTopCv(dest_in);
	if (dest->kind == TK_ARRAY)
		return result;  // only braced/string forms initialize arrays
	if (source.function_set)
		return ClassifyFunctionSet(source, dest);

	TypePtr from = DecayedValueType(source.type);
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
			return result;
		}
		// 4.10p3: pointer to derived converts to pointer to a (no less
		// qualified) base; nearer bases rank better (13.3.3.2p4).
		if (from->target->kind == TK_CLASS &&
		    dest->target->kind == TK_CLASS &&
		    CvSuperset(dest->target, from->target))
		{
			int distance = BaseClassDistance(from->target->named,
			                                 dest->target->named);
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
	if (dest->kind == TK_MEMBER_POINTER && source.null_pointer_literal)
	{
		result.viable = true;
		result.rank = CR_CONVERSION;
		result.null_to_pointer = true;
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
	// conversion (the PA15 user-defined-conversion subset).
	if (dest->kind == TK_CLASS && dest->named->class_record &&
	    !(from->kind == TK_CLASS &&
	      BaseClassDistance(from->named, dest->named) >= 0))
	{
		const ClassInfo& cls = *dest->named->class_record;
		for (size_t i = 0; i < cls.ctors.size(); i++)
		{
			const ClassCtor& ctor = cls.ctors[i];
			if (ctor.is_explicit || ctor.deleted ||
			    ctor.type->parameters.size() != 1)
				continue;
			const TypePtr& param = ctor.type->parameters[0];
			if (param->kind == TK_CLASS ||
			    (IsReferenceType(param) &&
			     param->target->kind == TK_CLASS))
				continue;  // no chained user conversions
			ImplicitConversion inner = ClassifyConversion(source, param);
			if (!inner.viable || inner.rank == CR_USER)
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
                                            const TypePtr& dest)
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
			SelectFunctionFromSet(source.overloads, referee, result);
		return result;
	}

	bool function_source = source.type->kind == TK_FUNCTION;
	bool lvalue_source = source.category == VC_LVALUE;
	bool rvalue_source = !lvalue_source;
	if (rvalue_ref)
	{
		// 8.5.3p5: an rvalue reference binds rvalues and function
		// lvalues only.
		if (lvalue_source && !function_source)
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
	}
	// Not reference-related: a const lvalue reference (or an rvalue
	// reference over an rvalue) binds a temporary created by the value
	// conversion, which carries the conversion's rank (13.3.3.1.4p2).
	bool referee_const = false;
	bool referee_volatile = false;
	TopCv(referee, referee_const, referee_volatile);
	bool const_lvalue_ref = !rvalue_ref && referee_const && !referee_volatile;
	if (!const_lvalue_ref && !(rvalue_ref && rvalue_source))
		return result;
	ImplicitConversion value =
		ClassifyValueConversion(source, RemoveTopCv(referee));
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
	if (a.bool_from_pointer != b.bool_from_pointer)
		return a.bool_from_pointer ? 1 : -1;
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
		// p3: the less cv-qualified referee wins.
		if (CvSuperset(b.referee, a.referee))
			return -1;
		if (CvSuperset(a.referee, b.referee))
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
ImplicitConversion ClassifySourceConversionFunction(
	const ConversionSource& source, const TypePtr& dest, bool contextual)
{
	ImplicitConversion result;
	TypePtr from = RemoveTopCv(source.type);
	if (from->kind != TK_CLASS || !from->named->class_record)
		return result;
	bool dest_bool = !IsReferenceType(dest) && IsBoolType(RemoveTopCv(dest));
	ImplicitConversion best_object;
	for (const ClassInfo* link = from->named->class_record; link;
	     link = link->base)
	{
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
				source, MakeReferenceType(object_class, false, true));
			if (!object.viable)
				continue;
			// One standard conversion from the result to `dest`.
			ConversionSource inner;
			inner.type = IsReferenceType(conv.result)
				? conv.result->target : RemoveTopCv(conv.result);
			inner.category = conv.result->kind == TK_LVALUE_REFERENCE
				? VC_LVALUE : VC_PRVALUE;
			ImplicitConversion second = ClassifyConversion(inner, dest);
			if (!second.viable || second.rank == CR_USER)
				continue;
			bool better = false;
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
			}
			if (!better)
				continue;
			result.viable = true;
			result.rank = CR_USER;
			result.conv_class = link->entity;
			result.conv_index = (int)i;
			result.second_rank = second.rank;
			result.null_to_pointer = false;
			best_object = object;
		}
	}
	return result;
}

}  // namespace

ImplicitConversion ClassifyConversion(const ConversionSource& source,
                                      const TypePtr& dest)
{
	return ClassifyConversionEx(source, dest, false);
}

ImplicitConversion ClassifyConversionEx(const ConversionSource& source,
                                        const TypePtr& dest,
                                        bool contextual)
{
	ImplicitConversion result = IsReferenceType(dest)
		? ClassifyReferenceBinding(source, dest)
		: ClassifyValueConversion(source, dest);
	if (result.viable)
		return result;
	if (source.type && RemoveTopCv(source.type)->kind == TK_CLASS &&
	    !source.function_set)
		return ClassifySourceConversionFunction(source, dest, contextual);
	return result;
}

size_t SelectBestOverload(const vector<TypePtr>& candidates,
                          const vector<ConversionSource>& args,
                          vector<ImplicitConversion>& conversions,
                          const vector<size_t>* min_arity)
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
		throw runtime_error("no matching function for call");
	size_t best = 0;
	for (size_t i = 1; i < viable.size(); i++)
		if (BetterCandidate(viable[i], viable[best], args))
			best = i;
	for (size_t i = 0; i < viable.size(); i++)
		if (i != best && !BetterCandidate(viable[best], viable[i], args))
			throw runtime_error("ambiguous overloaded call");
	conversions = viable[best].conversions;
	return viable[best].index;
}
