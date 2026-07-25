#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// Shared helpers owned by template_args.cpp.
const AstName* PlainExprName(const AstExpr& expr);
EFundamentalType ValueTargetFundamental(const TypePtr& type);

// PA20+ template-argument constant evaluation: converted constant
// expressions toward value parameters (5.19p3/14.3.2), constexpr
// member-call and class-conversion gates, and default value-argument
// resolution. Split from template_args.cpp, which keeps parameter
// collection and argument-list resolution.

// The single returned expression of an in-class function body, or
// null when the body has any other shape.
static const AstExpr* SingleReturnExpr(const AstDecl* decl)
{
	if (!decl || !decl->body || decl->body->kind != SK_COMPOUND ||
	    decl->body->items.size() != 1)
		return 0;
	const AstStmt& stmt = *decl->body->items[0];
	if (stmt.kind != SK_RETURN || !stmt.expr)
		return 0;
	return stmt.expr.get();
}

// PA20: the full engine evaluates a converted constant expression
// (5.19p3) toward an integral/enum template-parameter type. Class
// operands convert through their (constexpr) conversion functions;
// non-constexpr conversions fail, which 14.3.2 requires.
// `Check::ok()` in template-argument position parses as a function
// type-id; the value re-read deduces the named static constexpr
// member template with no arguments and evaluates its single-return
// body under the specialization's argument scope.
bool SemBinder::EvaluateZeroArgConstantCall(const AstTypeId& type_id,
                                            ConstValue& out)
{
	if (type_id.specifiers.size() != 1 ||
	    type_id.specifiers[0].kind != SPEC_TYPE_NAME ||
	    !type_id.declarator)
		return false;
	const AstDeclarator& declarator = *type_id.declarator;
	if (declarator.items.size() != 1 ||
	    declarator.items[0].kind != DI_PARAMS ||
	    !declarator.items[0].params ||
	    !declarator.items[0].params->parameters.empty())
		return false;
	const AstName& callee = type_id.specifiers[0].name;
	if (callee.parts.empty())
		return false;
	const ScopeBinding* binding = 0;
	const NamedTypeInfo* member_class = 0;
	try
	{
		binding = ResolveValue(callee, member_class);
	}
	catch (const std::exception&)
	{
		return false;
	}
	if (!binding || binding->kind != SB_FUNCTION)
		return false;
	// A fully-resolved template-id (the instantiation seam already
	// selected the specialization, `C::template f<A, B>`) evaluates
	// directly.
	if (binding->fn_self_spec)
		return EvaluateConstexprSpecReturn(*binding->fn_self_spec, out);
	const AstNamePart* explicit_part =
		callee.parts.back().kind == NP_TEMPLATE_ID
			? &callee.parts.back() : 0;
	for (size_t t = 0; t < binding->fn_templates.size(); t++)
	{
		const FunctionSpecialization* spec = DeduceFunctionTemplate(
			*binding->fn_templates[t], vector<SemValue>(),
			explicit_part);
		if (!spec || !spec->owner || !spec->owner->pattern_decl)
			continue;
		if (!DeclHasConstexpr(*spec->owner->pattern_decl))
			continue;
		return EvaluateConstexprSpecReturn(*spec, out);
	}
	// PA39: an ordinary (non-template) static constexpr member behind
	// a typedef-spelled qualifier parses as a function type-id too
	// (`_Node_alloc_traits::_S_nothrow_move()`): evaluate the unique
	// zero-parameter overload through the full engine over its
	// analyzed definition.
	TypePtr zero_arg;
	if (binding->type && binding->type->kind == TK_FUNCTION &&
	    binding->type->parameters.empty() && !binding->type->variadic)
		zero_arg = binding->type;
	for (size_t i = 0; i < binding->overloads.size(); i++)
		if (binding->overloads[i]->kind == TK_FUNCTION &&
		    binding->overloads[i]->parameters.empty() &&
		    !binding->overloads[i]->variadic)
		{
			if (zero_arg)
				return false;
			zero_arg = binding->overloads[i];
		}
	if (!zero_arg || !binding->owner)
		return false;
	SemNodePtr callee_node = MakeSemNode(SN_CALLEE);
	callee_node->name = CanonicalQualifiedName(binding->owner,
	                                           binding->name);
	callee_node->type = zero_arg;
	callee_node->entity_scope = binding->owner;
	callee_node->entity_name = binding->name;
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = zero_arg->target;
	call->category = VC_PRVALUE;
	call->children.push_back(std::move(callee_node));
	try
	{
		out = engine_.EvaluateIntegral(*call);
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

// The constant value of a constexpr specialization's lone return
// expression, evaluated under its argument bindings.
bool SemBinder::EvaluateConstexprSpecReturn(
	const FunctionSpecialization& spec, ConstValue& out)
{
	if (!spec.owner || !spec.owner->pattern_decl ||
	    !DeclHasConstexpr(*spec.owner->pattern_decl))
		return false;
	const AstExpr* ret = SingleReturnExpr(spec.owner->pattern_decl);
	if (!ret)
		return false;
	Scope* saved = current_;
	current_ = spec.param_scope;
	bool evaluated = false;
	try
	{
		out = EvaluateConstExpr(*ret, *this);
		evaluated = true;
	}
	catch (const std::exception&)
	{
		try
		{
			evaluated = TryFullConstant(*ret, out);
		}
		catch (const std::exception&)
		{
		}
	}
	current_ = saved;
	return evaluated;
}

bool SemBinder::TryFullValueArgument(const AstExpr& expr,
                                     const TypePtr& param_type,
                                     ConstValue& out)
{
	if (InAbstractTemplateContext())
		return false;
	try
	{
		SemValue value = analyzer_.Analyze(expr);
		if (value.type && RemoveTopCv(value.type)->kind == TK_CLASS)
			analyzer_.CopyInitialize(value, RemoveTopCv(param_type),
			                         "template argument");
		EvalValue result = engine_.EvaluateScalar(*value.node);
		if (result.kind != EvalValue::EV_INT)
			return false;
		out = result.ival;
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

// The restricted constexpr-conversion evaluation behind `B{}` in a
// constant context: a conversion function of B (or a base) to an
// integral type whose body is a single return of a constant
// expression, evaluated in its class scope. The temporary's own state
// cannot influence the result within this subset - a body that reads
// the object fails the inner evaluation and falls out of the subset
// naturally.
bool SemBinder::TryClassConversionConstant(const AstName& name,
                                           ConstValue& out)
{
	TypePtr type = TryResolveTypeFromName(name);
	// PA20 14.3.2/5.19: only a constexpr conversion function produces
	// a converted constant expression.
	return ClassConversionConstant(type, true, out);
}

// The reference dialect treats the libstdc++ helper
// `_TCC<Cond>::__is_implicitly_constructible<Args...>()` as an
// intrinsic: the result is the enclosing specialization's leading
// bool template argument, and the captured body never runs. The
// qualifier must resolve to a class-template specialization declaring
// a member function template of that name.
bool SemBinder::TupleConstraintGate(const AstName& name, ConstValue& out)
{
	Scope* scope = 0;
	try
	{
		scope = ResolvePrefixScope(name);
	}
	catch (const std::exception&)
	{
		return false;
	}
	if (!scope || scope->kind != SCOPE_CLASS || !scope->entity)
		return false;
	const NamedTypeInfo* entity = scope->entity;
	if (!entity->spec_template || entity->is_template_anchor ||
	    entity->spec_args.empty())
		return false;
	const ScopeBinding* member = FindOwnBinding(
		*scope, "__is_implicitly_constructible");
	if (!member || member->kind != SB_FUNCTION ||
	    member->fn_templates.empty())
		return false;
	const TemplateArg& gate = entity->spec_args.front();
	if (!gate.is_value || gate.value_type != FT_BOOL)
		return false;
	out = ConstValue(FT_BOOL, gate.value_bits);
	return true;
}

bool SemBinder::TryConstantClassBool(const TypePtr& type, bool& out)
{
	// The presentation fold for `flag ? a : b` over an
	// integral_constant-style object: the reference folds through a
	// plain (non-constexpr) conversion operator whose body returns a
	// constant.
	ConstValue value;
	if (!ClassConversionConstant(type, false, value))
		return false;
	out = value.bits != 0;
	return true;
}

bool SemBinder::ClassConversionConstant(const TypePtr& type,
                                        bool require_constexpr,
                                        ConstValue& out)
{
	if (!type || type->kind != TK_CLASS)
		return false;
	EnsureTypeCompleteness(type->named);
	for (const NamedTypeInfo* entity = type->named; entity;
	     entity = entity->base_entity)
	{
		const ClassInfo* cls = unit_.classes.Find(entity);
		if (!cls)
			continue;
		for (size_t i = 0; i < cls->conversions.size(); i++)
		{
			const ClassConversion& conv = cls->conversions[i];
			if (!conv.decl)
				continue;
			if (require_constexpr && !DeclHasConstexpr(*conv.decl))
				continue;
			TypePtr result = RemoveTopCv(conv.result);
			if (!IsIntegralType(result) && result->kind != TK_ENUM)
				continue;
			const AstExpr* returned = SingleReturnExpr(conv.decl);
			if (!returned)
				continue;
			// The presentation fold applies only to a returned NAME (a
			// static constant member); a returned literal keeps its
			// runtime call (both shapes are pinned).
			if (!require_constexpr)
			{
				const AstExpr* inner = returned;
				while (inner->kind == EK_PAREN &&
				       inner->operands.size() == 1)
					inner = inner->operands[0].get();
				if (inner->kind != EK_ID)
					continue;
			}
			Scope* saved = current_;
			current_ = cls->members;
			try
			{
				ConstValue value = EvaluateConstExpr(*returned, *this);
				out = ConvertConstValue(
					value,
					result->kind == TK_ENUM
						? result->named->enum_underlying
						: result->fundamental);
			}
			catch (const std::exception&)
			{
				current_ = saved;
				continue;
			}
			current_ = saved;
			return true;
		}
	}
	return false;
}

// One defaulted value argument: evaluated in the (already swapped-in)
// partial binding scope.
TemplateArg SemBinder::ResolveDefaultValueExpr(const AstExpr& expr,
                                               const TypePtr& param_type)
{
	TemplateArg arg;
	arg.is_value = true;
	arg.type = param_type;
	// 14.3.2: a pointer parameter's null default (`nullptr`, `0`);
	// entity-address defaults resolve like written arguments.
	if (param_type && param_type->kind == TK_POINTER)
	{
		bool null_form = expr.kind == EK_KEYWORD_LITERAL &&
			expr.literal == "nullptr";
		if (!null_form)
			try
			{
				ConstValue value = EvaluateConstExpr(expr, *this);
				null_form = value.bits == 0;
			}
			catch (const std::exception&)
			{
			}
		if (!null_form)
			throw runtime_error("pointer parameter default is not "
			                    "a null pointer constant");
		arg.value_type = FT_UNSIGNED_LONG_INT;
		arg.value_bits = 0;
		return arg;
	}
	if (!TypeIsDependent(param_type))
	{
		try
		{
			ConstValue value = EvaluateConstExpr(expr, *this);
			value = ConvertConstValue(
				value, ValueTargetFundamental(param_type));
			arg.value_type = value.type;
			arg.value_bits = value.bits;
			return arg;
		}
		catch (const std::exception&)
		{
			ConstValue value;
			if (TryFullValueArgument(expr, param_type, value))
			{
				value = ConvertConstValue(
					value, ValueTargetFundamental(param_type));
				arg.value_type = value.type;
				arg.value_bits = value.bits;
				return arg;
			}
			if (!InAbstractTemplateContext())
				throw;
		}
	}
	else if (!InAbstractTemplateContext())
		throw runtime_error("value parameter type did not resolve");
	if (const AstName* name = PlainExprName(expr))
	{
		const ScopeBinding* found = ResolveTerminal(*name, SLF_ANY);
		if (found && found->kind == SB_VARIABLE && found->no_object &&
		    found->param_index >= 0)
		{
			arg.value_param = found->param_index;
			return arg;
		}
	}
	arg.dependent_value = &expr;
	return arg;
}

