#include "sema/const_expr.h"

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
	default:
		return 64;
	}
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
	default:
		return 3;  // long long int / unsigned long long int
	}
}

EFundamentalType UnsignedOfRank(int rank)
{
	return rank == 1 ? FT_UNSIGNED_INT :
		rank == 2 ? FT_UNSIGNED_LONG_INT : FT_UNSIGNED_LONG_LONG_INT;
}

EFundamentalType SignedOfRank(int rank)
{
	return rank == 1 ? FT_INT :
		rank == 2 ? FT_LONG_INT : FT_LONG_LONG_INT;
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
		return Normalize(promoted.type, 0 - promoted.bits);
	}
	case OP_COMPL:
	{
		ConstValue promoted = Promote(operand);
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
	if (op == OP_LSHIFT)
		return Normalize(value.type, value.bits << count);
	if (IsSignedIntegralFundamental(value.type))
		return Normalize(value.type,
		                 (unsigned long long)((long long)value.bits >> count));
	// Unsigned values are stored zero-extended, so the logical shift
	// needs no masking.
	return Normalize(value.type, value.bits >> count);
}

ConstValue EvaluateArithmetic(ETokenType op, const ConstValue& lhs,
                              const ConstValue& rhs)
{
	EFundamentalType common =
		CommonType(Promote(lhs).type, Promote(rhs).type);
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
	if (!IsIntegralType(target))
		throw OutsideSubset("cast to a non-integral type");
	ConstValue operand = EvaluateConstExpr(*expr.operands[0], context);
	return ConvertConstValue(operand, target->fundamental);
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
			context.RequireCompleteForLayout(named);
			return ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(named));
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
	return Normalize(to, value.bits);
}

bool ConstValueIsNonZero(const ConstValue& value)
{
	return value.bits != 0;
}

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
		context.RequireCompleteForLayout(sized);
		return ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(sized));
	}
	case EK_SIZEOF_EXPR:
		return EvaluateSizeofExpr(expr, context);
	case EK_CALL:
		return EvaluateGnuAlignof(expr, context);
	case EK_TYPE_TRAIT:
		return EvaluateTypeTrait(expr, context);
	default:
		throw OutsideSubset("expression form");
	}
}
