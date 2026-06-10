#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "ast/ast.h"
#include "sema/scope.h"
#include "sema/sem_convert.h"
#include "sema/sem_node.h"

// PA12 expression analysis: resolves a PA10 expression tree against the
// PA11 scope/type model, producing the dump node plus the (type, value
// category) facts the enclosing context composes with. The analyzer
// reaches the binder through ISemExprHost - name and type resolution
// stay in the binder, the clause 4/5/13 rules stay here.

// The analyzer's view of the PA12 binder.
struct ISemExprHost
{
	virtual Scope* CurrentScope() = 0;
	virtual TypesModel& Model() = 0;
	// Resolution of a possibly qualified value name; `member_class` is
	// set when the binding is a class member (null otherwise). Throws
	// when the name does not resolve.
	virtual const ScopeBinding* ResolveValue(
		const AstName& name, const NamedTypeInfo*& member_class) = 0;
	// The type a callee name denotes, or null when it does not name a
	// type (functional-cast disambiguation).
	virtual TypePtr TryResolveCalleeType(const AstName& name) = 0;
	// 8.4 type-id resolution (casts, sizeof).
	virtual TypePtr ResolveCastTypeId(const AstTypeId& type_id) = 0;
	// Constant evaluation attempt for the __builtin_constant_p fold.
	virtual bool TryEvaluateConstant(const AstExpr& expr,
	                                 ConstValue& value) = 0;
	virtual ~ISemExprHost() {}
};

// One analyzed expression: the dump node plus composition facts. The
// node's printed type may keep a reference (call results, casts to
// reference types); `type` is always the reference-stripped type the
// enclosing expression computes with.
struct SemValue
{
	SemValue()
		: category(VC_PRVALUE), null_pointer_literal(false),
		  function_set(false), fn_owner(0), member_class(0)
	{}

	SemNodePtr node;
	TypePtr type;
	EValueCategory category;
	bool null_pointer_literal;  // literal of integer type, value 0
	// An id-expression naming one or more functions (resolution may be
	// target-directed, 13.4).
	bool function_set;
	vector<TypePtr> overloads;
	const Scope* fn_owner;  // declaring scope (canonical callee name)
	string fn_name;
	// Set when the id names a class member: the class entity and the
	// declared member type (member-function types keep their trailing
	// cv here; the node shows the this-adjusted spelling).
	const NamedTypeInfo* member_class;
	TypePtr member_type;
};

// The canonical qualified name of a binding declared in `owner`: the
// named namespace path from the global scope (unnamed namespace
// components are skipped) joined with `::`.
string CanonicalQualifiedName(const Scope* owner, const string& name);

class SemExprAnalyzer
{
public:
	explicit SemExprAnalyzer(ISemExprHost& host);

	SemValue Analyze(const AstExpr& expr);

	// 8.5 copy-initialization of a `dest`-typed object, parameter, or
	// return value (references bind): validates the conversion, retypes
	// null pointer literals, and resolves target-directed function
	// sets. Throws when no conversion exists.
	void CopyInitialize(SemValue& value, const TypePtr& dest,
	                    const char* what);

	// Braced initialization of the supported array forms; returns the
	// braced-init-list node (completing unknown bounds via `dest`).
	SemNodePtr AnalyzeBracedInit(const AstExpr& braced, TypePtr& dest);

	void RequireContextualBool(const SemValue& value, const char* what);

private:
	SemValue AnalyzeLiteral(const AstExpr& expr);
	SemValue AnalyzeKeywordLiteral(const AstExpr& expr);
	SemValue AnalyzeId(const AstExpr& expr);
	SemValue AnalyzeCall(const AstExpr& expr);
	SemValue AnalyzeNamedCall(const AstExpr& expr,
	                          const ScopeBinding& binding,
	                          const NamedTypeInfo* member_class);
	SemValue AnalyzeIndirectCall(const AstExpr& expr);
	SemValue AnalyzeBuiltinConstantP(const AstExpr& expr);
	SemValue AnalyzeUnary(const AstExpr& expr);
	SemValue AnalyzeAddressOf(const AstExpr& expr);
	SemValue AnalyzeIncDec(const AstExpr& expr, bool prefix);
	SemValue AnalyzeBinary(const AstExpr& expr);
	SemValue AnalyzeAdditive(const AstExpr& expr, SemValue& lhs,
	                         SemValue& rhs);
	SemValue AnalyzeComparison(const AstExpr& expr, SemValue& lhs,
	                           SemValue& rhs);
	SemValue AnalyzeAssignment(const AstExpr& expr);
	SemValue AnalyzeConditional(const AstExpr& expr);
	TypePtr ConditionalResultType(const SemValue& a, const SemValue& b,
	                              EValueCategory& category);
	SemValue AnalyzeSubscript(const AstExpr& expr);
	SemValue AnalyzeMember(const AstExpr& expr);
	SemValue AnalyzeCastTo(const TypePtr& dest, const AstExpr& operand,
	                       bool has_anno, ETokenType op,
	                       const string& op_spelling);
	SemValue AnalyzeFunctionalCast(const TypePtr& dest,
	                               const vector<AstExprPtr>& arguments);
	SemValue AnalyzeSizeof(const AstExpr& expr);
	SemValue CallResult(const TypePtr& function_type);
	SemValue MakeBinaryNode(const AstExpr& expr, SemValue& lhs,
	                        SemValue& rhs, EValueCategory category,
	                        const TypePtr& type);
	void CheckCallArguments(const TypePtr& function_type,
	                        vector<SemValue>& args);
	TypePtr CompositePointerType(const SemValue& a, const SemValue& b);
	void RequireModifiableLvalue(const SemValue& value, const char* what);
	void ApplyConversion(SemValue& value, const ImplicitConversion& conv,
	                     const TypePtr& dest);
	TypePtr ThisAdjustedType(const NamedTypeInfo* cls,
	                         const TypePtr& member) const;

	ISemExprHost& host_;
};

// The conversion-relevant facts of an analyzed value.
ConversionSource MakeConversionSource(const SemValue& value);
