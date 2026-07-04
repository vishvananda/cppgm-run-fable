#include "sema/sem_expr.h"

#include <stdexcept>

#include "sema/const_eval.h"

using std::runtime_error;

// PA12 explicit conversions: cast expressions (5.2.9/5.2.10/5.2.11
// over the supported subset), functional casts and constructed
// temporaries, and the sizeof / alignof constant forms.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

const AstExpr* StripParens(const AstExpr* expr)
{
	while (expr->kind == EK_PAREN)
		expr = expr->operands[0].get();
	return expr;
}

bool IsArithmeticOrEnum(const TypePtr& type)
{
	return IsArithmeticType(type) || type->kind == TK_ENUM;
}

bool IsPointerAfterDecay(const TypePtr& type)
{
	return type->kind == TK_POINTER || type->kind == TK_ARRAY;
}

// 5.2.11p4 similarity for const_cast: the same type ignoring
// cv-qualification at every level.
bool SimilarTypes(const TypePtr& a, const TypePtr& b)
{
	TypePtr sa = RemoveTopCv(a);
	TypePtr sb = RemoveTopCv(b);
	if (sa->kind != sb->kind)
		return false;
	switch (sa->kind)
	{
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
		return SimilarTypes(sa->target, sb->target);
	case TK_ARRAY:
		return sa->bound_known == sb->bound_known &&
			sa->bound == sb->bound &&
			SimilarTypes(sa->target, sb->target);
	default:
		return TypeEquals(sa, sb);
	}
}

}  // namespace

// 5.2.9p2-p4 / 5.2.11: a cast to reference type adjusts the operand's
// category and printed type; no cast node is dumped. const_cast
// accepts the 5.2.11 similar types.
SemValue SemExprAnalyzer::AnalyzeCastToReference(const TypePtr& dest,
                                                 SemValue value,
                                                 ETokenType op)
{
	const TypePtr& referee = dest->target;
	bool compatible = op == KW_CONST_CAST
		? SimilarTypes(referee, value.type)
		: TypeEquals(RemoveTopCv(referee), RemoveTopCv(value.type));
	if (!value.function_set && !compatible &&
	    op != KW_CONST_CAST &&
	    (dest->kind == TK_RVALUE_REFERENCE ||
	     (referee->is_const && !referee->is_volatile)))
	{
		// 5.2.9p4: the cast direct-initializes a temporary bound
		// to the reference.
		ImplicitConversion conv = ClassifyConversionEx(
			MakeConversionSource(value), RemoveTopCv(referee), true);
		if (conv.viable &&
		    (conv.user_ctor >= 0 || conv.conv_index >= 0))
		{
			ApplyConversion(value, conv, RemoveTopCv(referee));
			value.category = dest->kind == TK_RVALUE_REFERENCE
				? VC_XVALUE : VC_LVALUE;
			return value;
		}
	}
	if (!value.function_set && !compatible && op != KW_CONST_CAST)
	{
		// 5.2.9p2: a cast to a (no less cv-qualified) base
		// reference views the base-class subobject.
		TypePtr from = RemoveTopCv(value.type);
		TypePtr to = RemoveTopCv(referee);
		int hops = from->kind == TK_CLASS && to->kind == TK_CLASS
			? BaseClassDistance(from->named, to->named) : -1;
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = to;
			adjusted->category = value.category == VC_PRVALUE
				? VC_XVALUE : value.category;
			adjusted->base_hops = hops;
			adjusted->children.push_back(std::move(value.node));
			value.node = std::move(adjusted);
			value.type = to;
			value.category = dest->kind == TK_RVALUE_REFERENCE
				? VC_XVALUE : VC_LVALUE;
			return value;
		}
		// 5.2.9p2/p11 with op != KW_CONST_CAST: a downcast reference
		// static_cast views the derived object (the single
		// -inheritance base sits at offset 0, so no adjustment).
		int down = from->kind == TK_CLASS && to->kind == TK_CLASS
			? BaseClassDistance(to->named, from->named) : -1;
		if (down > 0 && op == KW_STATIC_CAST)
		{
			value.type = to;
			if (value.node)
				value.node->type = to;
			value.category = dest->kind == TK_RVALUE_REFERENCE
				? VC_XVALUE : VC_LVALUE;
			return value;
		}
	}
	if (value.function_set || !compatible)
		throw OutsideBoundary("reference cast form");
	if (dest->kind == TK_LVALUE_REFERENCE &&
	    value.category != VC_LVALUE)
		throw runtime_error("lvalue reference cast of an rvalue");
	value.category = dest->kind == TK_RVALUE_REFERENCE
		? VC_XVALUE : VC_LVALUE;
	value.type = referee;
	value.node->category = value.category;
	value.node->type = dest;
	value.null_pointer_literal = false;
	return value;
}

SemValue SemExprAnalyzer::AnalyzeCastTo(const TypePtr& dest,
                                        const AstExpr& operand,
                                        bool has_anno, ETokenType op,
                                        const string& op_spelling)
{
	SemValue value = Analyze(operand);
	if (IsReferenceType(dest))
		return AnalyzeCastToReference(dest, std::move(value), op);
	if (value.function_set && value.overloads.size() > 1)
		throw OutsideBoundary("cast of an overloaded name");
	TypePtr to = RemoveTopCv(dest);
	if (to->kind != TK_CLASS && !value.function_set &&
	    RemoveTopCv(value.type)->kind == TK_CLASS)
	{
		// 5.2.9p4: a cast is direct-initialization; explicit
		// conversion functions participate.
		ImplicitConversion conv = ClassifyConversionEx(
			MakeConversionSource(value), to, true);
		if (conv.viable && conv.conv_index >= 0)
			ApplyConversion(value, conv, to);
	}
	if (to->kind == TK_CLASS)
	{
		// 5.2.9p4 / 5.4: a cast to a class type direct-initializes a
		// temporary from the operand (explicit constructors allowed).
		const ClassInfo* cls = host_.Classes().Find(to->named);
		if (!cls || !to->named->complete)
			throw runtime_error("cast to an incomplete class");
		vector<SemValue> args;
		args.push_back(std::move(value));
		int index = host_.ResolveClassCtorHost(*cls, args, false, "cast");
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < args.size(); i++)
			arg_nodes.push_back(std::move(args[i].node));
		SemNodePtr action = host_.MakeConstructorCall(
			*cls, index, false, SemNodePtr(), std::move(arg_nodes));
		action->type = to;
		action->category = VC_PRVALUE;
		if (host_.Classes().NeedsDestruction(*cls))
		{
			action->needs_dtor = true;
			action->children.push_back(host_.MakeTemporaryDtor(*cls));
		}
		SemValue result;
		result.type = to;
		result.category = VC_PRVALUE;
		result.node = std::move(action);
		return result;
	}
	bool valid;
	if (IsVoidType(to))
		valid = true;
	else if (IsArithmeticType(to))
		valid = IsArithmeticOrEnum(value.type) ||
			((to->fundamental == FT_BOOL) &&
			 (IsPointerAfterDecay(value.type) ||
			  IsNullPtrType(value.type) ||
			  value.type->kind == TK_MEMBER_POINTER));
	else if (to->kind == TK_ENUM)
		valid = IsArithmeticOrEnum(value.type);
	else if (to->kind == TK_POINTER)
		valid = value.null_pointer_literal || IsNullPtrType(value.type) ||
			IsPointerAfterDecay(value.type) ||
			// 5.2.10p5: reinterpret_cast of an integral value.
			(op == KW_REINTERPET_CAST && IsIntegralType(value.type));
	else
		valid = false;
	if (!valid)
		throw OutsideBoundary("cast form");

	SemValue result;
	result.type = to;
	result.category = VC_PRVALUE;
	result.node = MakeSemNode(SN_CAST_EXPRESSION);
	result.node->type = to;
	result.node->category = VC_PRVALUE;
	result.node->has_op = has_anno;
	result.node->op = op;
	result.node->op_spelling = op_spelling;
	result.node->children.push_back(std::move(value.node));
	return result;
}

SemValue SemExprAnalyzer::AnalyzeFunctionalCast(
	const TypePtr& dest, const vector<AstExprPtr>& arguments)
{
	if (RemoveTopCv(dest)->kind == TK_CLASS)
		// T(args) over a class: a constructed temporary object.
		return MakeTemporaryObject(RemoveTopCv(dest), arguments, false);
	if (RemoveTopCv(dest)->kind == TK_ARRAY)
	{
		// 5.2.3p3: `T{...}` over an array type direct-list-initializes
		// a temporary array (the parser routes the braced form through
		// the call shape).
		TypePtr array = RemoveTopCv(dest);
		SemValue result;
		result.node = AnalyzeBracedInit(arguments, array);
		result.node->category = VC_PRVALUE;
		result.type = array;
		result.category = VC_PRVALUE;
		return result;
	}
	// PA19: pack expansions among the arguments resolve first; the
	// expanded count selects the form.
	bool has_pack = false;
	for (size_t i = 0; i < arguments.size(); i++)
		if (arguments[i]->kind == EK_PACK_EXPANSION)
			has_pack = true;
	if (has_pack)
	{
		vector<SemValue> values;
		AnalyzeArgumentList(arguments, values);
		if (values.size() > 1)
			throw OutsideBoundary("multi-argument functional cast");
		if (values.size() == 1)
		{
			// Direct-initialization of the scalar from the expanded
			// value.
			ImplicitConversion conv = ClassifyConversionEx(
				MakeConversionSource(values[0]), dest, true);
			if (!conv.viable)
				throw runtime_error("no conversion in functional cast");
			ApplyConversion(values[0], conv, dest);
			return std::move(values[0]);
		}
		// An empty expansion value-initializes (the form below).
	}
	else if (arguments.size() == 1)
		return AnalyzeCastTo(dest, *arguments[0], false, OP_LPAREN, "");
	else if (!arguments.empty())
		throw OutsideBoundary("multi-argument functional cast");
	// 5.2.3p2: T() value-initializes; the supported scalar subset dumps
	// as a zero literal.
	TypePtr to = RemoveTopCv(dest);
	if (to->kind == TK_POINTER || IsNullPtrType(to))
	{
		// 8.5p8: a value-initialized pointer is null.
		SemValue value;
		value.type = to;
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_LITERAL);
		value.node->token = "0";
		value.node->type = to;
		value.node->category = VC_PRVALUE;
		value.node->null_pointer = true;
		return value;
	}
	// 5.2.3p2: `void()` is a prvalue of type void (an unevaluated
	// -operand probe shape).
	if (IsVoidType(to))
	{
		SemValue value;
		value.type = to;
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_CAST_EXPRESSION);
		value.node->type = to;
		value.node->category = VC_PRVALUE;
		return value;
	}
	if (!IsIntegralType(to) && to->kind != TK_ENUM)
		throw OutsideBoundary("value-initialization form");
	SemValue value;
	value.type = to;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = "0";
	value.node->type = to;
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(
		to->kind == TK_ENUM ? to->named->enum_underlying : to->fundamental,
		0);
	value.null_pointer_literal = IsIntegralType(to);
	return value;
}

// 5.3.7: noexcept(expression) folds from the resolved unwind facts of
// the unevaluated operand - false iff it contains a potentially
// -evaluated call to a callee without a non-throwing specification.
SemValue SemExprAnalyzer::AnalyzeNoexcept(const AstExpr& expr)
{
	if (expr.is_type_operand || expr.operands.empty())
		throw OutsideBoundary("noexcept operand form");
	bool saved = host_.SwapUnevaluatedOperand(true);
	SemValue operand;
	try
	{
		operand = Analyze(*expr.operands[0]);
	}
	catch (...)
	{
		host_.SwapUnevaluatedOperand(saved);
		throw;
	}
	host_.SwapUnevaluatedOperand(saved);
	bool no_throw = !SemTreeMayThrow(*operand.node) &&
		(!operand.node->result_dtor ||
		 !SemTreeMayThrow(*operand.node->result_dtor));
	SemValue value;
	value.type = MakeFundamentalType(FT_BOOL);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_BOOL, no_throw ? 1 : 0);
	value.node->token = no_throw ? "true" : "false";
	return value;
}

SemValue SemExprAnalyzer::AnalyzeSizeof(const AstExpr& expr)
{
	bool alignment = expr.kind == EK_TYPE_TRAIT;
	if (alignment && expr.op == KW_NOEXCEPT)
		return AnalyzeNoexcept(expr);
	if (alignment && expr.op != KW_ALIGNOF)
		throw OutsideBoundary("type trait expression");
	TypePtr operand_type;
	if (expr.kind == EK_SIZEOF_TYPE || (alignment && expr.is_type_operand))
		operand_type = host_.ResolveCastTypeId(*expr.type);
	else
	{
		// `sizeof(x)` parses as an expression; a bare type-name operand
		// still disambiguates to the type form semantically.
		const AstExpr* operand = StripParens(expr.operands[0].get());
		if (operand->kind == EK_ID)
			operand_type = host_.TryResolveCalleeType(operand->name);
		if (!operand_type)
		{
			// 5.3.3p1: the operand is unevaluated (no odr-use).
			bool saved = host_.SwapUnevaluatedOperand(true);
			try
			{
				operand_type = Analyze(*expr.operands[0]).type;
			}
			catch (...)
			{
				host_.SwapUnevaluatedOperand(saved);
				throw;
			}
			host_.SwapUnevaluatedOperand(saved);
		}
	}
	// 5.3.3p2 / 3.11: applied to a reference type, the result is that
	// of the referenced type.
	if (IsReferenceType(operand_type))
		operand_type = operand_type->target;
	// 5.3.3p1 / 5.3.6p1: requires a complete object type; the size (or
	// alignment) is the value. PA18: deferred nested member-class
	// definitions complete here.
	TypePtr sizeof_bare = operand_type;
	while (sizeof_bare->kind == TK_ARRAY)
		sizeof_bare = sizeof_bare->target;
	if (sizeof_bare->kind == TK_CLASS)
		host_.RequireCompleteType(sizeof_bare->named);
	unsigned long long size = alignment ? TypeAlignment(operand_type)
	                                    : TypeSize(operand_type);
	SemValue value;
	value.type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_SIZEOF_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_UNSIGNED_LONG_INT, size);
	return value;
}

