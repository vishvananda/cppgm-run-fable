#pragma once

#include "ast/ast.h"
#include "sema/scope.h"

// The PA11 integral constant-expression subset (5.19) over the PA10
// expression tree: integer/character/boolean literals, id-expressions
// naming enumerators or const integral variables with recorded
// constant initializers, the unary/binary/conditional operators,
// integral casts, and the type-forming sizeof/alignof cases. Anything
// outside the subset throws (the inputs are then outside the PA11
// boundary, which the failing fixtures expect to exit EXIT_FAILURE).
//
// The evaluator sees the binder through this interface, keeping the
// declaration machinery and the evaluation rules in separate modules.
struct IConstExprContext
{
	// The constant value of an id-expression (throws when the name does
	// not resolve to an enumerator or recorded constant).
	virtual ConstValue LookupConstant(const AstName& name) = 0;
	// The type an id-expression names, or null when it names no type
	// (the 5.3.3 semantic disambiguation of `sizeof(T)`).
	virtual TypePtr TryResolveTypeFromName(const AstName& name) = 0;
	virtual TypePtr ResolveTypeId(const AstTypeId& type_id) = 0;
	// PA18: completes a deferred member-class definition before a
	// sizeof/alignof reads its layout (no-op outside templates).
	virtual void RequireCompleteForLayout(const TypePtr& type) {}
	// PA18: the type of an unevaluated operand (5.3.3p1 sizeof over a
	// general expression); null when the context cannot analyze
	// expressions or the operand is ill-formed.
	virtual TypePtr TryAnalyzeExpressionType(const AstExpr& expr)
	{
		(void)expr;
		return TypePtr();
	}
	// PA19: the constant value of a class-typed functional cast
	// (`B{}` / `B(x)`) through a conversion function whose body is a
	// single return of a constant expression. False when the name is
	// not such a class or no conversion of the subset applies.
	virtual bool TryClassConversionConstant(const AstName& name,
	                                        ConstValue& out)
	{
		(void)name;
		(void)out;
		return false;
	}
	virtual ~IConstExprContext() {}
};

ConstValue EvaluateConstExpr(const AstExpr& expr, IConstExprContext& context);

// 4.5/4.7 on constant values: integral promotion / conversion to a
// (possibly narrower) integral type. Throws for non-integral targets.
ConstValue ConvertConstValue(const ConstValue& value, EFundamentalType to);

bool ConstValueIsNonZero(const ConstValue& value);
