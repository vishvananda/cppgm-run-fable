#include "sema/const_expr.h"

#include "sema/sem_trait.h"

#include <stdexcept>

#include "numeric_literals.h"
#include "text_literals.h"

using std::runtime_error;

namespace {

runtime_error OutsideSubset(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA11 constant-expression subset");
}

int WidthBits(EFundamentalType type)
{
	switch (type)
	{
	case FT_BOOL:
	case FT_CHAR:
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
		return 8;
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:
		return 16;
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_WCHAR_T:
	case FT_CHAR32_T:
		return 32;
	case FT_INT128:
	case FT_UINT128:
		return 128;
	default:
		return 64;
	}
}

// PA34: 128-bit constants compute in the host-independent packed
// form (the pair rides ConstValue.high/bits).
typedef __int128 SWide;
typedef unsigned __int128 UWide;

UWide PackWide(const ConstValue& value)
{
	if (WidthBits(value.type) > 64)
		return ((UWide)value.high << 64) | value.bits;
	if (IsSignedIntegralFundamental(value.type))
		return (UWide)(SWide)(long long)value.bits;
	return (UWide)value.bits;
}

ConstValue MakeWide(EFundamentalType type, UWide value)
{
	return ConstValue(type, (unsigned long long)value,
	                  (unsigned long long)(value >> 64));
}

// Truncates to the type's width and re-extends to the 64-bit storage
// form (sign-extended for signed types).
ConstValue Normalize(EFundamentalType type, unsigned long long bits)
{
	int width = WidthBits(type);
	if (type == FT_BOOL)
		bits = bits != 0 ? 1 : 0;
	else if (width < 64)
	{
		unsigned long long mask = (1ull << width) - 1;
		bits &= mask;
		if (IsSignedIntegralFundamental(type) &&
		    (bits & (1ull << (width - 1))))
			bits |= ~mask;
	}
	return ConstValue(type, bits);
}

// 4.5: integral promotion.
ConstValue Promote(const ConstValue& value)
{
	switch (value.type)
	{
	case FT_BOOL:
	case FT_CHAR:
	case FT_SIGNED_CHAR:
	case FT_UNSIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T:
	case FT_WCHAR_T:
		return Normalize(FT_INT, value.bits);
	case FT_CHAR32_T:
		return Normalize(FT_UNSIGNED_INT, value.bits);
	default:
		return value;
	}
}

// Conversion rank of the promoted types (4.13).
int PromotedRank(EFundamentalType type)
{
	switch (type)
	{
	case FT_INT:
	case FT_UNSIGNED_INT:
		return 1;
	case FT_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
		return 2;
	case FT_INT128:
	case FT_UINT128:
		return 4;
	default:
		return 3;  // long long int / unsigned long long int
	}
}

EFundamentalType UnsignedOfRank(int rank)
{
	return rank == 1 ? FT_UNSIGNED_INT :
		rank == 2 ? FT_UNSIGNED_LONG_INT :
		rank == 4 ? FT_UINT128 : FT_UNSIGNED_LONG_LONG_INT;
}

EFundamentalType SignedOfRank(int rank)
{
	return rank == 1 ? FT_INT :
		rank == 2 ? FT_LONG_INT :
		rank == 4 ? FT_INT128 : FT_LONG_LONG_INT;
}

// 5p9 usual arithmetic conversions on promoted integral operands.
EFundamentalType CommonType(EFundamentalType a, EFundamentalType b)
{
	if (a == b)
		return a;
	bool a_signed = IsSignedIntegralFundamental(a);
	bool b_signed = IsSignedIntegralFundamental(b);
	int a_rank = PromotedRank(a);
	int b_rank = PromotedRank(b);
	if (a_signed == b_signed)
		return a_rank >= b_rank
			? (a_signed ? SignedOfRank(a_rank) : UnsignedOfRank(a_rank))
			: (b_signed ? SignedOfRank(b_rank) : UnsignedOfRank(b_rank));
	int signed_rank = a_signed ? a_rank : b_rank;
	int unsigned_rank = a_signed ? b_rank : a_rank;
	if (unsigned_rank >= signed_rank)
		return UnsignedOfRank(unsigned_rank);
	// LP64: a higher-rank signed type represents every value of the
	// lower-rank unsigned type (64/32), so the signed type wins.
	if (WidthBits(SignedOfRank(signed_rank)) >
	    WidthBits(UnsignedOfRank(unsigned_rank)))
		return SignedOfRank(signed_rank);
	return UnsignedOfRank(signed_rank);
}

ConstValue MakeBool(bool value)
{
	return ConstValue(FT_BOOL, value ? 1 : 0);
}

ConstValue EvaluateLiteral(const string& spelling)
{
	PostToken token;
	if (spelling.find('\'') != string::npos)
	{
		CharLiteralAnalysis analysis = AnalyzeCharLiteral(spelling);
		token = analysis.token;
	}
	else
		token = AnalyzePPNumber(spelling);
	if (token.kind != PTK_LITERAL || !IsIntegralFundamental(token.type))
		throw OutsideSubset("non-integral literal");
	return Normalize(token.type, LittleEndianValue(token.data));
}

ConstValue EvaluateUnary(const AstExpr& expr, IConstExprContext& context)
{
	if (expr.operands.size() != 1)
		throw OutsideSubset("unary operator form");
	ConstValue operand = EvaluateConstExpr(*expr.operands[0], context);
	switch (expr.op)
	{
	case OP_PLUS:
		return Promote(operand);
	case OP_MINUS:
	{
		ConstValue promoted = Promote(operand);
		if (WidthBits(promoted.type) > 64)
			return MakeWide(promoted.type, 0 - PackWide(promoted));
		return Normalize(promoted.type, 0 - promoted.bits);
	}
	case OP_COMPL:
	{
		ConstValue promoted = Promote(operand);
		if (WidthBits(promoted.type) > 64)
			return MakeWide(promoted.type, ~PackWide(promoted));
		return Normalize(promoted.type, ~promoted.bits);
	}
	case OP_LNOT:
		return MakeBool(!ConstValueIsNonZero(operand));
	default:
		throw OutsideSubset("unary operator");
	}
}

ConstValue EvaluateShift(ETokenType op, const ConstValue& lhs,
                         const ConstValue& rhs)
{
	ConstValue value = Promote(lhs);
	ConstValue amount = Promote(rhs);
	long long count = (long long)amount.bits;
	if (IsSignedIntegralFundamental(amount.type) && count < 0)
		throw runtime_error("negative shift count");
	if ((unsigned long long)count >= (unsigned)WidthBits(value.type))
		throw runtime_error("shift count out of range");
	if (WidthBits(value.type) > 64)
	{
		UWide wide = PackWide(value);
		if (op == OP_LSHIFT)
			return MakeWide(value.type, wide << count);
		if (IsSignedIntegralFundamental(value.type))
			return MakeWide(value.type, (UWide)((SWide)wide >> count));
		return MakeWide(value.type, wide >> count);
	}
	if (op == OP_LSHIFT)
		return Normalize(value.type, value.bits << count);
	if (IsSignedIntegralFundamental(value.type))
		return Normalize(value.type,
		                 (unsigned long long)((long long)value.bits >> count));
	// Unsigned values are stored zero-extended, so the logical shift
	// needs no masking.
	return Normalize(value.type, value.bits >> count);
}

// The 5p9 operators over packed 128-bit operands.
ConstValue EvaluateWideArithmetic(ETokenType op, EFundamentalType common,
                                  UWide a, UWide b)
{
	bool is_signed = IsSignedIntegralFundamental(common);
	switch (op)
	{
	case OP_STAR:
		return MakeWide(common, a * b);
	case OP_DIV:
	case OP_MOD:
	{
		if (b == 0)
			throw runtime_error("division by zero in constant "
			                    "expression");
		UWide quotient;
		UWide remainder;
		if (is_signed)
		{
			quotient = (UWide)((SWide)a / (SWide)b);
			remainder = (UWide)((SWide)a % (SWide)b);
		}
		else
		{
			quotient = a / b;
			remainder = a % b;
		}
		return MakeWide(common, op == OP_DIV ? quotient : remainder);
	}
	case OP_PLUS:
		return MakeWide(common, a + b);
	case OP_MINUS:
		return MakeWide(common, a - b);
	case OP_AMP:
		return MakeWide(common, a & b);
	case OP_XOR:
		return MakeWide(common, a ^ b);
	case OP_BOR:
		return MakeWide(common, a | b);
	case OP_LT:
		return MakeBool(is_signed ? (SWide)a < (SWide)b : a < b);
	case OP_GT:
		return MakeBool(is_signed ? (SWide)a > (SWide)b : a > b);
	case OP_LE:
		return MakeBool(is_signed ? (SWide)a <= (SWide)b : a <= b);
	case OP_GE:
		return MakeBool(is_signed ? (SWide)a >= (SWide)b : a >= b);
	case OP_EQ:
		return MakeBool(a == b);
	case OP_NE:
		return MakeBool(a != b);
	default:
		throw OutsideSubset("binary operator");
	}
}

ConstValue EvaluateArithmetic(ETokenType op, const ConstValue& lhs,
                              const ConstValue& rhs)
{
	EFundamentalType common =
		CommonType(Promote(lhs).type, Promote(rhs).type);
	if (WidthBits(common) > 64)
		return EvaluateWideArithmetic(op, common, PackWide(lhs),
		                              PackWide(rhs));
	unsigned long long a = Normalize(common, lhs.bits).bits;
	unsigned long long b = Normalize(common, rhs.bits).bits;
	bool is_signed = IsSignedIntegralFundamental(common);
	switch (op)
	{
	case OP_STAR:
		return Normalize(common, a * b);
	case OP_DIV:
	case OP_MOD:
	{
		if (b == 0)
			throw runtime_error("division by zero in constant expression");
		unsigned long long quotient;
		unsigned long long remainder;
		if (is_signed)
		{
			quotient = (unsigned long long)((long long)a / (long long)b);
			remainder = (unsigned long long)((long long)a % (long long)b);
		}
		else
		{
			quotient = a / b;
			remainder = a % b;
		}
		return Normalize(common, op == OP_DIV ? quotient : remainder);
	}
	case OP_PLUS:
		return Normalize(common, a + b);
	case OP_MINUS:
		return Normalize(common, a - b);
	case OP_AMP:
		return Normalize(common, a & b);
	case OP_XOR:
		return Normalize(common, a ^ b);
	case OP_BOR:
		return Normalize(common, a | b);
	case OP_LT:
		return MakeBool(is_signed ? (long long)a < (long long)b : a < b);
	case OP_GT:
		return MakeBool(is_signed ? (long long)a > (long long)b : a > b);
	case OP_LE:
		return MakeBool(is_signed ? (long long)a <= (long long)b : a <= b);
	case OP_GE:
		return MakeBool(is_signed ? (long long)a >= (long long)b : a >= b);
	case OP_EQ:
		return MakeBool(a == b);
	case OP_NE:
		return MakeBool(a != b);
	default:
		throw OutsideSubset("binary operator");
	}
}

ConstValue EvaluateBinary(const AstExpr& expr, IConstExprContext& context)
{
	if (expr.operands.size() != 2)
		throw OutsideSubset("binary operator form");
	// 5.14/5.15 short-circuit forms never evaluate the unused operand.
	if (expr.op == OP_LAND || expr.op == OP_LOR)
	{
		bool lhs = ConstValueIsNonZero(
			EvaluateConstExpr(*expr.operands[0], context));
		if (expr.op == OP_LAND ? !lhs : lhs)
			return MakeBool(lhs);
		return MakeBool(ConstValueIsNonZero(
			EvaluateConstExpr(*expr.operands[1], context)));
	}
	// PA21: the comma operator discards its left operand's value.
	if (expr.op == OP_COMMA)
	{
		EvaluateConstExpr(*expr.operands[0], context);
		return EvaluateConstExpr(*expr.operands[1], context);
	}
	ConstValue lhs = EvaluateConstExpr(*expr.operands[0], context);
	ConstValue rhs = EvaluateConstExpr(*expr.operands[1], context);
	if (expr.op == OP_LSHIFT || expr.op == OP_RSHIFT)
		return EvaluateShift(expr.op, lhs, rhs);
	return EvaluateArithmetic(expr.op, lhs, rhs);
}

// static_cast / C-style casts: the target must resolve to an integral
// type within the PA11 model.
ConstValue EvaluateTypedCast(const AstExpr& expr, IConstExprContext& context)
{
	if (expr.kind == EK_KEYWORD_CAST && expr.op != KW_STATIC_CAST)
		throw OutsideSubset("cast keyword");
	if (!expr.type || expr.operands.size() != 1)
		throw OutsideSubset("cast form");
	TypePtr target = context.ResolveTypeId(*expr.type);
	EFundamentalType fundamental;
	if (IsIntegralType(target))
		fundamental = target->fundamental;
	else if (target->kind == TK_ENUM)
		fundamental = target->named->enum_underlying;
	else if (IsVoidType(target))
	{
		// PA21: a discarded-value void cast inside a comma operand
		// (`(void(T()), 0)`); the operand need not fold.
		return ConstValue(FT_INT, 0);
	}
	else
		throw OutsideSubset("cast to a non-integral type");
	ConstValue operand = EvaluateConstExpr(*expr.operands[0], context);
	return ConvertConstValue(operand, fundamental);
}

EFundamentalType FunctionalCastTarget(ETokenType keyword)
{
	switch (keyword)
	{
	case KW_BOOL: return FT_BOOL;
	case KW_CHAR: return FT_CHAR;
	case KW_WCHAR_T: return FT_WCHAR_T;
	case KW_CHAR16_T: return FT_CHAR16_T;
	case KW_CHAR32_T: return FT_CHAR32_T;
	case KW_SHORT: return FT_SHORT_INT;
	case KW_INT: return FT_INT;
	case KW_SIGNED: return FT_INT;
	case KW_UNSIGNED: return FT_UNSIGNED_INT;
	case KW_LONG: return FT_LONG_INT;
	default:
		throw OutsideSubset("functional cast target");
	}
}

ConstValue EvaluateFunctionalCast(const AstExpr& expr,
                                  IConstExprContext& context)
{
	if (expr.arguments.size() != 1)
		throw OutsideSubset("functional cast form");
	ConstValue operand = EvaluateConstExpr(*expr.arguments[0], context);
	return ConvertConstValue(operand, FunctionalCastTarget(expr.op));
}

// The 5.3.3 semantic disambiguation: `sizeof ( expression )` where the
// expression is a (possibly parenthesized) id that lookup resolves to
// a type is sizeof(type). General sizeof-of-expression is outside the
// assignment boundary.
const AstName* UnparenthesizedIdName(const AstExpr& expr)
{
	const AstExpr* inner = &expr;
	while (inner->kind == EK_PAREN)
		inner = inner->operands[0].get();
	return inner->kind == EK_ID ? &inner->name : 0;
}

// The parenthesized sizeof operand resolved expression-first; an
// index chain over a type-name (`Elem<int>[2]`) semantically re-reads
// as an array type-id (5.3.3 disambiguation over the array form).
TypePtr TryArrayTypeFromSubscript(const AstExpr& expr,
                                  IConstExprContext& context)
{
	const AstExpr* inner = &expr;
	while (inner->kind == EK_PAREN)
		inner = inner->operands[0].get();
	std::vector<unsigned long long> bounds;
	while (inner->kind == EK_SUBSCRIPT && inner->operands.size() == 2)
	{
		bounds.push_back(
			EvaluateConstExpr(*inner->operands[1], context).bits);
		inner = inner->operands[0].get();
	}
	if (bounds.empty() || inner->kind != EK_ID)
		return TypePtr();
	TypePtr element = context.TryResolveTypeFromName(inner->name);
	if (!element)
		return TypePtr();
	TypePtr type = element;
	for (size_t i = 0; i < bounds.size(); i++)
		type = MakeArrayType(type, true, bounds[i]);
	return type;
}

ConstValue EvaluateSizeofExpr(const AstExpr& expr,
                              IConstExprContext& context)
{
	const AstName* name = expr.operands.empty()
		? 0 : UnparenthesizedIdName(*expr.operands[0]);
	if (expr.sizeof_paren && name)
	{
		TypePtr named = context.TryResolveTypeFromName(*name);
		if (named)
		{
			// 5.3.3p2: applied to a reference type, the result is
			// that of the referenced type.
			if (named->kind == TK_LVALUE_REFERENCE ||
			    named->kind == TK_RVALUE_REFERENCE)
				named = named->target;
			context.RequireCompleteForLayout(named);
			return ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(named));
		}
	}
	if (expr.sizeof_paren && !expr.operands.empty())
	{
		TypePtr array = TryArrayTypeFromSubscript(*expr.operands[0],
		                                          context);
		if (array)
		{
			TypePtr element = array;
			while (element->kind == TK_ARRAY)
				element = element->target;
			context.RequireCompleteForLayout(element);
			return ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(array));
		}
	}
	// 5.3.3p1: sizeof over an unevaluated expression operand.
	if (!expr.operands.empty())
	{
		TypePtr analyzed =
			context.TryAnalyzeExpressionType(*expr.operands[0]);
		if (analyzed)
		{
			if (IsReferenceType(analyzed))
				analyzed = analyzed->target;
			context.RequireCompleteForLayout(analyzed);
			return ConstValue(FT_UNSIGNED_LONG_INT,
			                  TypeSize(RemoveTopCv(analyzed)));
		}
	}
	throw OutsideSubset("sizeof of an expression");
}

// The GNU `__alignof(type)` extension parses as a call to an
// identifier; only that shape evaluates here.
ConstValue EvaluateGnuAlignof(const AstExpr& expr,
                              IConstExprContext& context)
{
	const AstExpr* callee = expr.operands.empty()
		? 0 : expr.operands[0].get();
	if (!callee || callee->kind != EK_ID ||
	    !callee->name.IsPlainIdentifier() ||
	    (callee->name.parts[0].identifier != "__alignof" &&
	     callee->name.parts[0].identifier != "__alignof__") ||
	    expr.arguments.size() != 1)
		throw OutsideSubset("expression form");
	const AstName* name = UnparenthesizedIdName(*expr.arguments[0]);
	TypePtr named = name ? context.TryResolveTypeFromName(*name)
	                     : TypePtr();
	if (!named)
		throw OutsideSubset("__alignof operand");
	context.RequireCompleteForLayout(named);
	return ConstValue(FT_UNSIGNED_LONG_INT, TypeAlignment(named));
}

// The 5.2.3 semantic disambiguation of `T(x)`: a call whose callee
// names a type is a functional cast; integral and enumeration targets
// fold within the subset.
ConstValue EvaluateCall(const AstExpr& expr, IConstExprContext& context)
{
	const AstExpr* callee = expr.operands.empty()
		? 0 : expr.operands[0].get();
	if (callee && callee->kind == EK_ID && expr.arguments.size() <= 1)
	{
		TypePtr named = context.TryResolveTypeFromName(callee->name);
		if (named && expr.arguments.size() == 1)
		{
			EFundamentalType target;
			if (IsIntegralType(named))
				target = named->fundamental;
			else if (named->kind == TK_ENUM)
				target = named->named->enum_underlying;
			else if (named->kind == TK_CLASS)
			{
				ConstValue out;
				if (context.TryClassConversionConstant(callee->name,
				                                       out))
					return out;
				throw OutsideSubset("class temporary value");
			}
			else
				throw OutsideSubset("functional cast to a "
				                    "non-integral type");
			return ConvertConstValue(
				EvaluateConstExpr(*expr.arguments[0], context), target);
		}
		if (named)
		{
			// Zero-argument value-initialization: integral targets are
			// zero; a class temporary's value comes through its
			// constant conversion function (`B{}` / `B()`).
			if (IsIntegralType(named))
				return ConstValue(named->fundamental, 0);
			if (named->kind == TK_ENUM)
				return ConstValue(named->named->enum_underlying, 0);
			ConstValue out;
			if (named->kind == TK_CLASS &&
			    context.TryClassConversionConstant(callee->name, out))
				return out;
			throw OutsideSubset("class temporary value");
		}
	}
	// The reference dialect's libstdc++ tuple-constraints intrinsic:
	// the exact helper name evaluates to the enclosing
	// specialization's bool gate instead of running the body.
	if (callee && callee->kind == EK_ID && expr.arguments.empty() &&
	    callee->name.parts.size() >= 2 &&
	    callee->name.parts.back().identifier ==
	        "__is_implicitly_constructible")
	{
		ConstValue gate;
		if (context.TupleConstraintGate(callee->name, gate))
			return gate;
	}
	return EvaluateGnuAlignof(expr, context);
}

ConstValue EvaluateTypeTrait(const AstExpr& expr,
                             IConstExprContext& context)
{
	if (expr.op != KW_ALIGNOF)
		throw OutsideSubset("type trait");
	if (expr.is_type_operand)
		return ConstValue(FT_UNSIGNED_LONG_INT,
		                  TypeAlignment(context.ResolveTypeId(*expr.type)));
	const AstName* name = expr.operands.empty()
		? 0 : UnparenthesizedIdName(*expr.operands[0]);
	TypePtr named = name ? context.TryResolveTypeFromName(*name) : TypePtr();
	if (named)
		return ConstValue(FT_UNSIGNED_LONG_INT, TypeAlignment(named));
	throw OutsideSubset("alignof of an expression");
}

}  // namespace

ConstValue ConvertConstValue(const ConstValue& value, EFundamentalType to)
{
	if (!IsIntegralFundamental(to))
		throw OutsideSubset("conversion to a non-integral type");
	// 128-bit targets extend by the source's signedness; narrower
	// targets truncate from the low word.
	if (WidthBits(to) > 64)
		return MakeWide(to, PackWide(value));
	return Normalize(to, value.bits);
}

ConstValue ConstIntPromote(const ConstValue& value)
{
	return Promote(value);
}

ConstValue ConstIntBinary(ETokenType op, const ConstValue& lhs,
                          const ConstValue& rhs)
{
	if (op == OP_LSHIFT || op == OP_RSHIFT)
		return EvaluateShift(op, lhs, rhs);
	return EvaluateArithmetic(op, lhs, rhs);
}

ConstValue ConstIntUnary(ETokenType op, const ConstValue& operand)
{
	ConstValue promoted = Promote(operand);
	switch (op)
	{
	case OP_PLUS:
		return promoted;
	case OP_MINUS:
		if (WidthBits(promoted.type) > 64)
			return MakeWide(promoted.type, 0 - PackWide(promoted));
		return Normalize(promoted.type, 0 - promoted.bits);
	case OP_COMPL:
		if (WidthBits(promoted.type) > 64)
			return MakeWide(promoted.type, ~PackWide(promoted));
		return Normalize(promoted.type, ~promoted.bits);
	case OP_LNOT:
		return MakeBool(!ConstValueIsNonZero(operand));
	default:
		throw OutsideSubset("unary operator");
	}
}

bool ConstValueIsNonZero(const ConstValue& value)
{
	return value.bits != 0 || value.high != 0;
}

static ConstValue EvaluateBuiltinTraitExpr(const AstExpr& expr,
                                           IConstExprContext& context);

ConstValue EvaluateConstExpr(const AstExpr& expr, IConstExprContext& context)
{
	switch (expr.kind)
	{
	case EK_LITERAL:
		return EvaluateLiteral(expr.literal);
	case EK_KEYWORD_LITERAL:
		if (expr.op == KW_TRUE)
			return MakeBool(true);
		if (expr.op == KW_FALSE)
			return MakeBool(false);
		throw OutsideSubset("keyword literal");
	case EK_ID:
		return context.LookupConstant(expr.name);
	case EK_PAREN:
		return EvaluateConstExpr(*expr.operands[0], context);
	case EK_UNARY:
		return EvaluateUnary(expr, context);
	case EK_BINARY:
		return EvaluateBinary(expr, context);
	case EK_CONDITIONAL:
		if (expr.operands.size() != 3)
			throw OutsideSubset("conditional form");
		return ConstValueIsNonZero(
			EvaluateConstExpr(*expr.operands[0], context))
			? EvaluateConstExpr(*expr.operands[1], context)
			: EvaluateConstExpr(*expr.operands[2], context);
	case EK_KEYWORD_CAST:
	case EK_CSTYLE_CAST:
		return EvaluateTypedCast(expr, context);
	case EK_FUNCTIONAL_CAST:
		return EvaluateFunctionalCast(expr, context);
	case EK_SIZEOF_TYPE:
	{
		TypePtr sized = context.ResolveTypeId(*expr.type);
		// 5.3.3p2: applied to a reference type, the result is that of
		// the referenced type.
		if (sized->kind == TK_LVALUE_REFERENCE ||
		    sized->kind == TK_RVALUE_REFERENCE)
			sized = sized->target;
		context.RequireCompleteForLayout(sized);
		return ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(sized));
	}
	case EK_SIZEOF_EXPR:
		return EvaluateSizeofExpr(expr, context);
	case EK_SIZEOF_PACK:
	{
		// PA21 14.5.3p5: sizeof...(pack) folds to the element count.
		unsigned long long count = 0;
		if (!expr.name.parts.empty() &&
		    context.PackSizeConstant(expr.name.parts[0].identifier,
		                             count))
			return ConstValue(FT_UNSIGNED_LONG_INT, count);
		throw OutsideSubset("sizeof... of a non-pack");
	}
	case EK_SUBSCRIPT:
	{
		// 5.19-adjacent: an element read of a string literal folds to
		// the code unit (the terminator included).
		if (expr.operands.size() != 2)
			throw OutsideSubset("subscript form");
		const AstExpr* base = expr.operands[0].get();
		while (base->kind == EK_PAREN)
			base = base->operands[0].get();
		if (base->kind != EK_LITERAL ||
		    base->literal_kind != PTK_LITERAL_ARRAY ||
		    !IsIntegralFundamental(base->literal_type))
			throw OutsideSubset("subscript of a non-literal");
		unsigned long long index =
			EvaluateConstExpr(*expr.operands[1], context).bits;
		size_t unit = WidthBits(base->literal_type) / 8;
		if (index >= base->literal_elements)
			throw runtime_error("string literal index out of range");
		unsigned long long bits = 0;
		for (size_t b = 0; b < unit; b++)
			bits |= (unsigned long long)(unsigned char)
				base->literal_data[index * unit + b] << (8 * b);
		return Normalize(base->literal_type, bits);
	}
	case EK_CALL:
		return EvaluateCall(expr, context);
	case EK_TYPE_TRAIT:
		return EvaluateTypeTrait(expr, context);
	case EK_BUILTIN_TRAIT:
		return EvaluateBuiltinTraitExpr(expr, context);
	default:
		throw OutsideSubset("expression form");
	}
}

// PA34 builtin type trait over resolved operand types (the PA11
// walker's slice; the analyzer's sem_trait.cpp path handles probe
// traits and pack-expanded arguments).
static ConstValue EvaluateBuiltinTraitExpr(const AstExpr& expr,
                                           IConstExprContext& context)
{
	std::vector<TypePtr> types;
	for (size_t i = 0; i < expr.trait_args.size(); i++)
	{
		if (expr.trait_args[i].pack)
			throw OutsideSubset("pack-expanded builtin trait "
			                    "argument");
		types.push_back(
			context.ResolveTypeId(*expr.trait_args[i].type));
		// A dependent operand defers: the argument slot re-reads
		// concretely at instantiation.
		if (TypeIsDependent(types.back()))
			throw OutsideSubset("dependent builtin trait operand");
	}
	// PA34 value trait: __array_rank(T) counts array dimensions.
	if (expr.op_spelling == "__array_rank")
	{
		TypePtr bare = RemoveTopCv(types[0]);
		unsigned long long rank = 0;
		while (bare->kind == TK_ARRAY)
		{
			rank++;
			bare = RemoveTopCv(bare->target);
		}
		return ConstValue(FT_UNSIGNED_LONG_INT, rank);
	}
	bool value = EvaluateBuiltinTraitOnTypes(
		expr.op_spelling, types,
		[&context](const TypePtr& type) {
			context.RequireCompleteForLayout(type);
		});
	return MakeBool(value);
}
