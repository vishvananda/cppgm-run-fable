#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA19 template parameters and arguments: collection of template
// heads (type and integral non-type parameters), parameter-driven
// resolution of template-argument lists (type-ids and integral
// constant expressions, defaults on the tail), and the alias bindings
// an instantiation scope carries for each parameter. Value arguments
// evaluate through the PA11 constant-expression machinery in the use
// scope; inside abstract pattern contexts an unevaluable argument
// becomes a dependent pattern slot instead of an error, and
// instantiation re-resolves it concretely.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA19 assignment boundary");
}

// The bare (possibly qualified) name of a type-id with no other
// specifiers and no declarator operators: the form that re-reads as an
// id-expression when the parameter wants a value (the parser prefers
// the type reading for `Box<A>`).
const AstName* BareTypeIdName(const AstTypeId& type_id)
{
	if (type_id.declarator && !type_id.declarator->Empty())
		return 0;
	if (type_id.specifiers.size() != 1 ||
	    type_id.specifiers[0].kind != SPEC_TYPE_NAME)
		return 0;
	return &type_id.specifiers[0].name;
}

// The unparenthesized id-expression under `expr`, when that is its
// whole shape (mirrors the constant evaluator's disambiguation).
const AstName* PlainExprName(const AstExpr& expr)
{
	const AstExpr* inner = &expr;
	while (inner->kind == EK_PAREN && inner->operands.size() == 1)
		inner = inner->operands[0].get();
	return inner->kind == EK_ID ? &inner->name : 0;
}

// 14.3: `bool(B::value)` parses as a function type-id; when the
// parameter wants a value it re-reads as a functional cast of the
// "parameter" name. Returns the operand name when the type-id has
// exactly that shape.
const AstName* FunctionalCastOperandName(const AstTypeId& type_id)
{
	if (!type_id.declarator || type_id.declarator->items.size() != 1)
		return 0;
	const AstDeclaratorItem& item = type_id.declarator->items[0];
	if (item.kind != DI_PARAMS || !item.params ||
	    item.params->parameters.size() != 1 || item.params->variadic)
		return 0;
	const AstParameter& parameter = item.params->parameters[0];
	if (parameter.declarator && !parameter.declarator->Empty())
		return 0;
	if (parameter.specifiers.size() != 1 ||
	    parameter.specifiers[0].kind != SPEC_TYPE_NAME)
		return 0;
	return &parameter.specifiers[0].name;
}

}  // namespace

void SemBinder::CollectTemplateParams(const AstDecl& decl,
                                      vector<TemplateParam>& params)
{
	for (size_t i = 0; i < decl.template_params.size(); i++)
	{
		const AstTemplateParameter& parameter = decl.template_params[i];
		TemplateParam param;
		switch (parameter.kind)
		{
		case TP_TYPE:
			param.kind = TPK_TYPE;
			param.name = parameter.name;
			if (parameter.has_default_type)
				param.default_type = parameter.default_type.get();
			break;
		case TP_NON_TYPE:
		{
			param.kind = TPK_VALUE;
			param.source = &parameter;
			if (parameter.declarator)
				if (const AstName* id = parameter.declarator->IdName())
					if (id->IsPlainIdentifier())
						param.name = id->parts[0].identifier;
			if (parameter.has_default_expr)
				param.default_expr = parameter.default_expr.get();
			break;
		}
		default:
			throw OutsideBoundary("template template parameter");
		}
		if (parameter.pack)
			throw OutsideBoundary("template parameter pack");
		param.pack = parameter.pack;
		params.push_back(param);
	}
}

// Whether the current lookup context is an abstract template pattern
// (placeholder type parameters or valueless value parameters in
// scope): unevaluable value arguments there are dependent, not
// ill-formed.
bool SemBinder::InAbstractTemplateContext() const
{
	for (const Scope* scope = current_; scope; scope = scope->parent)
	{
		if (scope->kind != SCOPE_TEMPLATE_PARAMS)
			continue;
		for (size_t i = 0; i < scope->bindings.size(); i++)
		{
			const ScopeBinding& binding = scope->bindings[i];
			if (binding.kind == SB_TYPE && binding.type &&
			    binding.type->kind == TK_TYPE_PARAM)
				return true;
			if (binding.kind == SB_TYPE_ALIAS &&
			    TypeIsDependent(binding.type))
				return true;
			if (binding.kind == SB_VARIABLE && binding.no_object &&
			    !binding.has_value)
				return true;
		}
	}
	return false;
}

// The declared type of one value parameter, composed under `bindings`
// (the earlier parameters' aliases) so dependent forms such as
// `template<class T, T v>` resolve. Integral, enumeration, and
// still-dependent types are the supported subset (14.1p4 restricted
// to the PA19 slice).
TypePtr SemBinder::ValueParamType(const TemplateParam& param,
                                  Scope* bindings)
{
	Scope* saved = current_;
	if (bindings)
		current_ = bindings;
	TypePtr type;
	try
	{
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(param.source->specifiers, false);
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			param.source->declarator.get(), specs.type);
		type = composed.type;
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	if (IsIntegralType(type) || type->kind == TK_ENUM ||
	    TypeIsDependent(type))
		return type;
	throw OutsideBoundary("non-integral non-type template parameter");
}

// The integral target of a value conversion: enumerations convert
// through their underlying type.
static EFundamentalType ValueTargetFundamental(const TypePtr& type)
{
	if (type->kind == TK_ENUM)
		return type->named->enum_underlying;
	return type->fundamental;
}

// Resolves one value argument (an expression, or a type-form argument
// re-read as an id-expression) against the parameter's declared type.
TemplateArg SemBinder::ResolveValueArgument(const AstTemplateArgument& argument,
                                            const TypePtr& param_type)
{
	const AstExpr* expr = argument.is_type ? 0 : argument.expr.get();
	const AstName* name = 0;
	const AstName* cast_operand = 0;
	if (argument.is_type)
	{
		name = BareTypeIdName(*argument.type);
		if (!name)
		{
			cast_operand = FunctionalCastOperandName(*argument.type);
			if (!cast_operand)
				throw runtime_error("template argument does not form "
				                    "a constant value");
		}
	}
	TemplateArg arg;
	arg.is_value = true;
	arg.type = param_type;
	if (!TypeIsDependent(param_type))
	{
		try
		{
			ConstValue value;
			if (expr)
				value = EvaluateConstExpr(*expr, *this);
			else if (name)
				value = LookupConstant(*name);
			else
			{
				// The functional-cast re-read: the target may be a
				// keyword type (spelled through the specifier seq).
				value = LookupConstant(*cast_operand);
				DeclSpecifierInfo target_specs =
					builder_.ProcessSpecifiers(argument.type->specifiers,
					                           false);
				value = ConvertConstValue(
					value, ValueTargetFundamental(target_specs.type));
			}
			value = ConvertConstValue(
				value, ValueTargetFundamental(param_type));
			arg.value_type = value.type;
			arg.value_bits = value.bits;
			return arg;
		}
		catch (const std::exception&)
		{
			if (!InAbstractTemplateContext())
				throw;
		}
	}
	else if (!InAbstractTemplateContext())
		throw runtime_error("value parameter type did not resolve");
	// A dependent pattern argument: a plain name that resolves to a
	// value parameter records its slot; anything else carries the
	// unevaluated expression for instantiation-time re-resolution.
	if (!name && expr)
		name = PlainExprName(*expr);
	if (name)
	{
		const ScopeBinding* found = ResolveTerminal(*name, SLF_ANY);
		if (found && found->kind == SB_VARIABLE && found->no_object &&
		    found->param_index >= 0)
		{
			arg.value_param = found->param_index;
			return arg;
		}
	}
	if (!expr)
		throw runtime_error("template argument does not form a "
		                    "constant value");
	arg.dependent_value = expr;
	return arg;
}

// Binds one parameter name to its argument in an alias scope: type
// arguments as type aliases, value arguments as objectless constants
// (reads fold; LookupConstant sees the value); pattern value slots
// keep their parameter marker.
void SemBinder::BindParamAlias(Scope& scope, const TemplateParam& param,
                               const TemplateArg& arg)
{
	if (param.name.empty())
		return;
	ScopeBinding alias;
	alias.name = param.name;
	if (param.kind == TPK_TYPE)
	{
		alias.kind = SB_TYPE_ALIAS;
		alias.type = arg.type;
	}
	else
	{
		alias.kind = SB_VARIABLE;
		alias.no_object = true;
		if (arg.type)
			alias.type = MakeCvQualifiedType(arg.type, true, false);
		if (arg.is_value && arg.value_param < 0 && !arg.dependent_value)
		{
			alias.has_value = true;
			alias.value = ConstValue(arg.value_type, arg.value_bits);
		}
		else
			alias.param_index = arg.value_param;
	}
	AddBinding(scope, alias);
}

// The lazily-created binding scope of one argument list: the earlier
// arguments' aliases, used by dependent value-parameter types and by
// default arguments (14.1: defaults resolve in the template's
// declaring context).
Scope* SemBinder::EnsureArgBindingScope(TemplateInfo& tmpl,
                                        const vector<TemplateArg>& so_far,
                                        Scope*& partial)
{
	if (!partial)
	{
		partial = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
		                             tmpl.declaring);
		for (size_t i = 0; i < so_far.size() && i < tmpl.params.size();
		     i++)
			BindParamAlias(*partial, tmpl.params[i], so_far[i]);
	}
	return partial;
}

vector<TemplateArg> SemBinder::ResolveTemplateArgumentList(
	TemplateInfo& tmpl, const AstNamePart& part)
{
	const std::vector<AstTemplateArgument>& source = part.arguments;
	if (source.size() > tmpl.params.size())
		throw runtime_error("too many template arguments for " +
		                    tmpl.name);
	vector<TemplateArg> args;
	Scope* partial = 0;
	for (size_t i = 0; i < tmpl.params.size(); i++)
	{
		const TemplateParam& param = tmpl.params[i];
		TemplateArg arg;
		if (i < source.size())
		{
			const AstTemplateArgument& argument = source[i];
			if (argument.pack)
				throw OutsideBoundary("pack-expansion template "
				                      "argument");
			if (param.kind == TPK_TYPE)
			{
				if (!argument.is_type || !argument.type)
					throw runtime_error("expected a type argument "
					                    "for " + tmpl.name);
				arg = TemplateArg(
					builder_.ResolveTypeId(*argument.type));
			}
			else
			{
				TypePtr param_type = ValueParamType(
					param,
					EnsureArgBindingScope(tmpl, args, partial));
				arg = ResolveValueArgument(argument, param_type);
			}
		}
		else
		{
			// Defaults resolve in the template's declaring scope with
			// the earlier parameters bound to the resolved arguments.
			EnsureArgBindingScope(tmpl, args, partial);
			if (param.kind == TPK_TYPE)
			{
				if (!param.default_type)
					throw runtime_error("too few template arguments "
					                    "for " + tmpl.name);
				Scope* saved = current_;
				current_ = partial;
				try
				{
					arg = TemplateArg(builder_.ResolveTypeId(
						*param.default_type));
				}
				catch (...)
				{
					current_ = saved;
					throw;
				}
				current_ = saved;
			}
			else
			{
				if (!param.default_expr)
					throw runtime_error("too few template arguments "
					                    "for " + tmpl.name);
				TypePtr param_type = ValueParamType(param, partial);
				Scope* saved = current_;
				current_ = partial;
				try
				{
					arg = ResolveDefaultValueExpr(*param.default_expr,
					                              param_type);
				}
				catch (...)
				{
					current_ = saved;
					throw;
				}
				current_ = saved;
			}
		}
		args.push_back(arg);
		if (partial)
			BindParamAlias(*partial, param, args.back());
	}
	return args;
}

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
			TypePtr result = RemoveTopCv(conv.result);
			if (!IsIntegralType(result) && result->kind != TK_ENUM)
				continue;
			const AstExpr* returned = SingleReturnExpr(conv.decl);
			if (!returned)
				continue;
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
