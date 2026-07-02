#include "sema/sem_expr.h"

#include <set>
#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA12 call analysis, split from sem_expr.cpp: named-call overload
// resolution (with the PA18 template candidates), indirect calls, and
// argument checking.

namespace {

const AstExpr* StripParens(const AstExpr* expr)
{
	while (expr->kind == EK_PAREN)
		expr = expr->operands[0].get();
	return expr;
}

}  // namespace

SemValue SemExprAnalyzer::AnalyzeCall(const AstExpr& expr)
{
	const AstExpr* callee = StripParens(expr.operands[0].get());
	if (callee->kind == EK_MEMBER)
		return AnalyzeMemberCall(expr, *callee);
	if (callee->kind == EK_ID)
	{
		// 3.4.2p1: a parenthesized callee suppresses argument-dependent
		// lookup.
		bool paren_callee = expr.operands[0]->kind == EK_PAREN;
		bool plain = callee->name.IsPlainIdentifier();
		if (TypePtr as_type = host_.TryResolveCalleeType(callee->name))
			return AnalyzeFunctionalCast(as_type, expr.arguments);
		const NamedTypeInfo* member_class = 0;
		const ScopeBinding* binding = 0;
		try
		{
			binding = host_.ResolveValue(callee->name, member_class);
		}
		catch (const AmbiguousLookupError&)
		{
			// 7.3.4p6: same-level function declarations form one
			// overload set rather than an ambiguity; any other
			// ambiguous lookup is ill-formed regardless of what a
			// builtin or ADL retry could supply (3.4.1p2).
			if (!plain || paren_callee)
				throw;
			vector<const ScopeBinding*> fn_set;
			UnqualifiedLookup(host_.CurrentScope(),
			                  callee->name.parts[0].identifier,
			                  SLF_ANY, &fn_set);
			return AnalyzeAdlCall(
				expr, callee->name.parts[0].identifier, fn_set);
		}
		catch (const std::exception&)
		{
			if (plain &&
			    callee->name.parts[0].identifier == "__builtin_constant_p")
				return AnalyzeBuiltinConstantP(expr);
			if (plain)
				binding = host_.ResolveBuiltinFunction(
					callee->name.parts[0].identifier);
			if (!binding && plain && !paren_callee)
				// Only argument-dependent lookup can name the callee
				// (hidden friends, associated namespaces).
				return AnalyzeAdlCall(
					expr, callee->name.parts[0].identifier,
					vector<const ScopeBinding*>());
			if (!binding)
				throw;
		}
		if (binding->kind == SB_FUNCTION)
		{
			if (plain && !paren_callee && binding->home &&
			    binding->home->kind == SCOPE_NAMESPACE)
				return AnalyzeAdlCall(
					expr, callee->name.parts[0].identifier,
					vector<const ScopeBinding*>(1, binding));
			// 10.3p15: explicit scope qualification suppresses the
			// virtual call mechanism.
			bool qualified = callee->name.parts.size() > 1 ||
				callee->name.global_scope;
			return AnalyzeNamedCall(expr, *binding, member_class,
			                        qualified);
		}
	}
	return AnalyzeIndirectCall(expr);
}

SemValue SemExprAnalyzer::AnalyzeNamedCall(const AstExpr& expr,
                                           const ScopeBinding& binding,
                                           const NamedTypeInfo* member_class,
                                           bool qualified)
{
	// A member function named without an object: bind the implicit
	// *this when the context provides one; otherwise only static
	// members are callable (9.3.1p3).
	const NamedTypeInfo* home_entity = 0;
	if (binding.home && binding.home->kind == SCOPE_CLASS)
		home_entity = host_.Model().ScopeEntity(binding.home);
	if (member_class || home_entity)
	{
		const NamedTypeInfo* target = member_class ? member_class
		                                           : home_entity;
		TypePtr this_type = host_.CurrentThisType();
		if (this_type &&
		    BaseClassDistance(this_type->target->named, target) >= 0)
		{
			SemValue object;
			object.node = ImplicitThisObject();
			object.type = object.node->type;
			object.category = VC_LVALUE;
			return AnalyzeMethodCall(std::move(object), binding,
			                         expr.arguments, qualified);
		}
		return AnalyzeStaticMethodCall(expr, binding);
	}
	vector<SemValue> args;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < expr.arguments.size(); i++)
	{
		args.push_back(Analyze(*expr.arguments[i]));
		sources.push_back(MakeConversionSource(args.back()));
	}
	vector<TypePtr> candidates;
	if (binding.type)
	{
		candidates.push_back(binding.type);
		for (size_t i = 0; i < binding.overloads.size(); i++)
			candidates.push_back(binding.overloads[i]);
	}
	// PA18: deduced function-template specializations join after the
	// ordinary overloads.
	// An explicit template-id callee pre-binds the leading template
	// parameters (14.8.1).
	const AstNamePart* explicit_part = 0;
	{
		const AstExpr* callee = StripParens(expr.operands[0].get());
		if (callee->kind == EK_ID && !callee->name.parts.empty() &&
		    callee->name.parts.back().kind == NP_TEMPLATE_ID)
			explicit_part = &callee->name.parts.back();
	}
	const size_t ordinary = candidates.size();
	vector<const FunctionSpecialization*> specs(ordinary, (const FunctionSpecialization*)0);
	{
		std::set<const void*> seen;
		vector<OperatorCandidate> deduced;
		AppendTemplateCandidates(binding, args, deduced, seen,
		                         explicit_part);
		for (size_t i = 0; i < deduced.size(); i++)
		{
			candidates.push_back(deduced[i].declared);
			specs.push_back(deduced[i].spec);
		}
	}
	// PA18 13.4: overloaded/template arguments deduce against every
	// candidate's parameter types before ranking.
	DeduceFunctionSetArguments(args, candidates, sources);
	vector<size_t> min_arity;
	vector<bool> is_template;
	NamedCallMinArity(binding, candidates, specs, ordinary, min_arity,
	                  is_template);
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(candidates, sources, conversions,
	                                   &min_arity, &is_template);
	const FunctionSpecialization* spec =
		winner < ordinary ? 0 : specs[winner];
	const ScopeBinding& chosen = spec ? spec->self : binding;
	const size_t slot = spec ? 0 : winner;
	if (slot < chosen.fn_deleted.size() && chosen.fn_deleted[slot])
		throw runtime_error("use of deleted function " + binding.name);
	const TypePtr& function_type = candidates[winner];
	for (size_t i = 0; i < args.size(); i++)
		if (i < function_type->parameters.size())
			ApplyConversion(args[i], conversions[i],
			                function_type->parameters[i]);
	// Synthesize the omitted trailing arguments from the recorded
	// default expressions (8.3.6p9: evaluated at the call site).
	for (size_t i = args.size(); i < function_type->parameters.size(); i++)
		args.push_back(SynthesizeDefaultArgument(
			chosen, slot, i, function_type->parameters[i]));

	SemValue value = CallResult(function_type);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(chosen.owner, chosen.name);
	callee->type = function_type;
	callee->entity_scope = chosen.owner;
	callee->entity_name = chosen.name;
	callee->fn_spec = spec;
	if (slot < chosen.fn_unwind_no.size() && chosen.fn_unwind_no[slot])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

void SemExprAnalyzer::CheckCallArguments(const TypePtr& function_type,
                                         vector<SemValue>& args)
{
	const vector<TypePtr>& params = function_type->parameters;
	if (args.size() < params.size() ||
	    (args.size() > params.size() && !function_type->variadic))
		throw runtime_error("wrong number of call arguments");
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i < params.size())
			CopyInitialize(args[i], params[i], "call argument");
		else if (args[i].function_set && args[i].overloads.size() > 1)
			throw runtime_error("overloaded name as ellipsis argument");
	}
}

SemValue SemExprAnalyzer::AnalyzeIndirectCall(const AstExpr& expr)
{
	SemValue fn = Analyze(*expr.operands[0]);
	if (fn.type->kind == TK_CLASS)
		return AnalyzeFunctorCall(std::move(fn), expr);
	TypePtr function_type;
	if (fn.type->kind == TK_FUNCTION)
		function_type = fn.type;
	else if (fn.type->kind == TK_POINTER &&
	         fn.type->target->kind == TK_FUNCTION)
		function_type = fn.type->target;
	else
		throw runtime_error("called object is not a function");
	if (fn.function_set && fn.overloads.size() > 1)
		throw runtime_error("overloaded name in a non-call context");

	vector<SemValue> args;
	for (size_t i = 0; i < expr.arguments.size(); i++)
		args.push_back(Analyze(*expr.arguments[i]));
	CheckCallArguments(function_type, args);

	SemValue value = CallResult(function_type);
	value.node->children.push_back(std::move(fn.node));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// 8.3.6: trailing default arguments lower each candidate's minimum
// call arity; deduced template specializations track their own facts.
void SemExprAnalyzer::NamedCallMinArity(
	const ScopeBinding& binding, const vector<TypePtr>& candidates,
	const vector<const FunctionSpecialization*>& specs, size_t ordinary,
	vector<size_t>& min_arity, vector<bool>& is_template)
{
	min_arity.assign(candidates.size(), 0);
	is_template.assign(candidates.size(), false);
	for (size_t c = 0; c < candidates.size(); c++)
	{
		size_t required = candidates[c]->parameters.size();
		const vector<const AstExpr*>* defaults = 0;
		if (c < ordinary)
			defaults = c < binding.fn_defaults.size()
				? &binding.fn_defaults[c] : 0;
		else
		{
			defaults = &specs[c]->self.fn_defaults[0];
			is_template[c] = true;
		}
		while (defaults && required > 0 &&
		       required <= defaults->size() && (*defaults)[required - 1])
			required--;
		min_arity[c] = required;
	}
}

// One omitted trailing argument (8.3.6p9: evaluated at the call site;
// instantiated signatures look their names up in their argument alias
// scope).
SemValue SemExprAnalyzer::SynthesizeDefaultArgument(
	const ScopeBinding& chosen, size_t slot, size_t index,
	const TypePtr& param)
{
	Scope* default_scope = TemplateDefaultArgScope(chosen);
	Scope* saved = default_scope
		? host_.SwapLookupScope(default_scope) : 0;
	SemValue filled;
	try
	{
		filled = Analyze(*chosen.fn_defaults[slot][index]);
	}
	catch (...)
	{
		if (default_scope)
			host_.SwapLookupScope(saved);
		throw;
	}
	if (default_scope)
		host_.SwapLookupScope(saved);
	CopyInitialize(filled, param, "default argument");
	return filled;
}
