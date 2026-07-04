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

// A `T...` template argument parses as a type-id whose abstract
// declarator carries the `...` as a DI_PACK item (the argument's own
// pack flag stays false); the composition filters the marker, so
// detection reads the declarator.
bool TypeIdHasPackMarker(const AstTypeId& type_id)
{
	if (!type_id.declarator)
		return false;
	for (size_t i = 0; i < type_id.declarator->items.size(); i++)
		if (type_id.declarator->items[i].kind == DI_PACK)
			return true;
	return false;
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

// One AST template-parameter list as TemplateParams (recursive for
// the nested lists of template-template parameters).
static void CollectParamList(
	const std::vector<AstTemplateParameter>& source,
	vector<TemplateParam>& params)
{
	for (size_t i = 0; i < source.size(); i++)
	{
		const AstTemplateParameter& parameter = source[i];
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
		case TP_TEMPLATE:
			// PA21: a template-template parameter; the nested list is
			// kept for arity/kind matching (14.3.3) and the default (a
			// template-name spelled as a type-id) resolves on demand.
			param.kind = TPK_TEMPLATE;
			param.name = parameter.name;
			if (parameter.has_default_type)
				param.default_type = parameter.default_type.get();
			CollectParamList(parameter.template_params, param.tt_params);
			break;
		}
		param.pack = parameter.pack;
		params.push_back(param);
	}
}

void SemBinder::CollectTemplateParams(const AstDecl& decl,
                                      vector<TemplateParam>& params)
{
	// PA21: multiple parameter packs are legal where each is deducible
	// (partial specializations, function templates); the class/variable
	// -template primary capture enforces the 14.1p11 trailing-pack rule
	// itself.
	CollectParamList(decl.template_params, params);
}

// The index of the parameter pack in `params`, or params.size() when
// there is none.
size_t TemplatePackIndex(const vector<TemplateParam>& params)
{
	for (size_t i = 0; i < params.size(); i++)
		if (params[i].pack)
			return i;
	return params.size();
}

// Maps a RESOLVED flattened argument list onto the parameter list:
// non-pack parameters take one argument each (in order, with any
// post-pack parameters occupying the tail) and the pack owns the
// middle run. Each parameter's span is [spans[i].first,
// spans[i].second). False when the counts cannot line up.
bool MapParamSpans(const vector<TemplateParam>& params, size_t argc,
                   vector<std::pair<size_t, size_t>>& spans)
{
	size_t pack_at = TemplatePackIndex(params);
	spans.clear();
	if (pack_at == params.size())
	{
		if (argc > params.size())
			return false;
		for (size_t i = 0; i < params.size(); i++)
			spans.push_back(std::make_pair(
				i < argc ? i : argc, i < argc ? i + 1 : argc));
		return true;
	}
	size_t trailing = params.size() - pack_at - 1;
	if (argc + 1 < params.size())
		return false;
	size_t middle = argc - pack_at - trailing;
	for (size_t i = 0; i < pack_at; i++)
		spans.push_back(std::make_pair(i, i + 1));
	spans.push_back(std::make_pair(pack_at, pack_at + middle));
	for (size_t i = 0; i < trailing; i++)
		spans.push_back(std::make_pair(pack_at + middle + i,
		                               pack_at + middle + i + 1));
	return true;
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
			    !binding.has_value &&
			    (!binding.is_pack || binding.param_index >= 0))
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
	// PA22: reference (and pointer) parameters take entity addresses
	// (14.3.2).
	if (IsReferenceType(type) || type->kind == TK_POINTER)
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
	// 14.3.2: a reference/pointer parameter binds the address of a
	// named entity with linkage; the argument spells the entity (with
	// an optional & for the pointer form).
	if (param_type &&
	    (IsReferenceType(param_type) || param_type->kind == TK_POINTER))
	{
		const AstName* entity = name;
		if (!entity && expr && expr->kind == EK_ID)
			entity = &expr->name;
		if (!entity && expr && expr->kind == EK_UNARY &&
		    expr->op_spelling == "&" && !expr->operands.empty() &&
		    expr->operands[0]->kind == EK_ID)
			entity = &expr->operands[0]->name;
		if (!entity)
			throw runtime_error("template argument does not name an "
			                    "entity with linkage");
		const ScopeBinding* found = ResolveTerminal(*entity, SLF_ANY);
		if (!found || found->kind != SB_VARIABLE || found->no_object ||
		    !found->owner || found->owner->kind != SCOPE_NAMESPACE)
			throw runtime_error("template argument does not name an "
			                    "entity with linkage");
		if (found->type &&
		    !TypeEquals(RemoveTopCv(found->type),
		                RemoveTopCv(param_type->target)))
			throw runtime_error("template argument entity type "
			                    "mismatch");
		arg.entity_scope = found->owner;
		arg.entity_name = found->name;
		return arg;
	}
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
			// PA20: the full engine covers constexpr calls, object
			// values, and class-to-integral constant conversions.
			ConstValue value;
			if (expr && TryFullValueArgument(*expr, param_type, value))
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

// --- definition-time template-id shape validation ---------------------------
//
// 14.3p1 structural checks a captured signature can fail without any
// instantiation: a pack-expansion argument must target a parameter
// pack. (Kind and arity mismatches that depend on resolved types stay
// instantiation-side.)

void SemBinder::ValidateTemplateIdShape(const AstNamePart& part)
{
	const ScopeBinding* found =
		UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found || found->kind != SB_CLASS_TEMPLATE || !found->templ)
		return;
	const vector<TemplateParam>& params = found->templ->params;
	size_t cursor = 0;
	for (size_t i = 0; i < part.arguments.size(); i++)
	{
		const AstTemplateArgument& argument = part.arguments[i];
		bool is_expansion = argument.pack ||
			(argument.is_type && argument.type &&
			 TypeIdHasPackMarker(*argument.type));
		if (cursor >= params.size())
			return;  // arity checks resolve at instantiation
		if (is_expansion && !params[cursor].pack)
			throw runtime_error("pack-expansion argument for a "
			                    "non-pack parameter of " +
			                    found->templ->name);
		if (!params[cursor].pack)
			cursor++;
	}
}

void SemBinder::ValidateSignatureTemplateIds(const AstSpecifierSeq& specifiers,
                                             const AstDeclarator* declarator)
{
	for (size_t i = 0; i < specifiers.size(); i++)
	{
		if (specifiers[i].kind != SPEC_TYPE_NAME)
			continue;
		const AstName& name = specifiers[i].name;
		for (size_t p = 0; p < name.parts.size(); p++)
		{
			const AstNamePart& part = name.parts[p];
			if (part.kind != NP_TEMPLATE_ID)
				continue;
			ValidateTemplateIdShape(part);
			for (size_t a = 0; a < part.arguments.size(); a++)
				if (part.arguments[a].type)
					ValidateSignatureTemplateIds(
						part.arguments[a].type->specifiers,
						part.arguments[a].type->declarator.get());
		}
	}
	if (!declarator)
		return;
	for (size_t i = 0; i < declarator->items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator->items[i];
		if (item.kind == DI_NESTED && item.nested)
			ValidateSignatureTemplateIds(AstSpecifierSeq(),
			                             item.nested.get());
		else if (item.kind == DI_PARAMS && item.params)
			for (size_t p = 0; p < item.params->parameters.size(); p++)
				ValidateSignatureTemplateIds(
					item.params->parameters[p].specifiers,
					item.params->parameters[p].declarator.get());
	}
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
	if (param.kind == TPK_TEMPLATE)
	{
		// PA21: the parameter name acts as a class-template name bound
		// to the argument's template entity.
		alias.kind = SB_CLASS_TEMPLATE;
		alias.templ = const_cast<TemplateInfo*>(arg.template_entity);
		if (!alias.templ)
			return;
		AddBinding(scope, alias);
		return;
	}
	if (param.kind == TPK_TYPE)
	{
		alias.kind = SB_TYPE_ALIAS;
		alias.type = arg.type;
	}
	else if (arg.entity_scope)
	{
		// PA22 entity-valued parameter (14.3.2): uses of the name are
		// uses of the named entity (the reference form reads/writes
		// the object directly; the alias resolves through the
		// entity's own scope and name).
		alias.kind = SB_VARIABLE;
		alias.type = arg.type && IsReferenceType(arg.type)
			? arg.type->target : arg.type;
		alias.owner = arg.entity_scope;
		alias.pack_element_name = arg.entity_name;
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

// Binds a parameter pack's alias: the run of elements it owns.
void SemBinder::BindPackAlias(Scope& scope, const TemplateParam& param,
                              const vector<TemplateArg>& args,
                              size_t begin, size_t end)
{
	vector<TemplateArg> elements;
	for (size_t i = begin; i < end && i < args.size(); i++)
		elements.push_back(args[i]);
	BindPackAliasElements(scope, param, elements);
}

// Binds a parameter pack's alias from an explicit element run (the
// deduced slot form of a partial specialization).
void SemBinder::BindPackAliasElements(Scope& scope,
                                      const TemplateParam& param,
                                      const vector<TemplateArg>& elements)
{
	if (param.name.empty())
		return;
	ScopeBinding alias;
	alias.name = param.name;
	alias.is_pack = true;
	if (param.kind == TPK_TYPE)
		alias.kind = SB_TYPE_ALIAS;
	else if (param.kind == TPK_TEMPLATE)
		alias.kind = SB_CLASS_TEMPLATE;
	else
	{
		alias.kind = SB_VARIABLE;
		alias.no_object = true;
	}
	alias.pack_args = elements;
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

// Resolves one source argument against a parameter's kind.
TemplateArg SemBinder::ResolveOneArgument(TemplateInfo& tmpl,
                                          const TemplateParam& param,
                                          const AstTemplateArgument& argument,
                                          const vector<TemplateArg>& so_far,
                                          Scope*& partial)
{
	if (param.kind == TPK_TEMPLATE)
	{
		// A template-name argument parses as a type-id spelling just
		// the name.
		const AstName* name = argument.is_type && argument.type
			? BareTypeIdName(*argument.type) : 0;
		if (!name)
			throw runtime_error("expected a template argument for " +
			                    tmpl.name);
		return ResolveTemplateTemplateArgument(*name, param, tmpl.name);
	}
	if (param.kind == TPK_TYPE)
	{
		if (!argument.is_type || !argument.type)
			throw runtime_error("expected a type argument for " +
			                    tmpl.name);
		try
		{
			return TemplateArg(builder_.ResolveTypeId(*argument.type));
		}
		catch (const std::exception&)
		{
			// PA21: a type-id needing instantiation-time facts
			// (dependent members, deferred aliases) becomes a pattern
			// slot re-resolved during specialization matching.
			if (!InAbstractTemplateContext())
				throw;
			TemplateArg deferred;
			deferred.dependent_type = argument.type.get();
			return deferred;
		}
	}
	TypePtr param_type = ValueParamType(
		param, EnsureArgBindingScope(tmpl, so_far, partial));
	return ResolveValueArgument(argument, param_type);
}

// One template-template argument: the named class template (or a
// bound template-template parameter, giving a dependent slot), arity
// -and-kind checked against the parameter's own list (14.3.3).
TemplateArg SemBinder::ResolveTemplateTemplateArgument(
	const AstName& name, const TemplateParam& param, const string& context)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	TemplateInfo* named = 0;
	if (found && found->kind == SB_CLASS_TEMPLATE)
		named = found->templ;
	// 14.6.1p2: the injected-class-name without an argument list acts
	// as the template-name.
	if (!named && found && found->kind == SB_TYPE && found->type &&
	    (found->type->kind == TK_CLASS ||
	     found->type->kind == TK_TEMPLATE_SPEC) &&
	    found->type->named->spec_template)
		named = const_cast<TemplateInfo*>(found->type->named->spec_template);
	if (!named)
		throw runtime_error("template argument of " + context +
		                    " does not name a template");
	TemplateArg arg;
	if (named->tt_param_index >= 0)
	{
		// A bound outer template-template parameter forwards its slot.
		arg.template_param = named->tt_param_index;
		return arg;
	}
	if (named->kind != TMPL_CLASS && named->kind != TMPL_ALIAS)
		throw runtime_error("template argument of " + context +
		                    " is not a class template");
	if (!TemplateTemplateParamsMatch(param.tt_params, named->params))
		throw runtime_error("template argument of " + context +
		                    " does not match the parameter's template "
		                    "parameter list");
	arg.template_entity = named;
	return arg;
}

// PA21: inside an abstract pattern, an array bound spelling a value
// template parameter composes as its slot (`T[N]`).
bool SemBinder::AbstractArrayBound(const AstExpr& expr, int& param)
{
	if (!InAbstractTemplateContext())
		return false;
	const AstName* name = PlainExprName(expr);
	if (!name)
		return false;
	const ScopeBinding* found = ResolveTerminal(*name, SLF_ANY);
	if (found && found->kind == SB_VARIABLE && found->no_object &&
	    !found->has_value && found->param_index >= 0)
	{
		param = found->param_index;
		return true;
	}
	return false;
}

// A pattern-scope placeholder template standing for the enclosing
// template's template-template parameter `index`; deduction reads the
// slot off the anchor entity.
TemplateInfo* SemBinder::TemplateParamPlaceholder(const TemplateParam& param,
                                                  size_t index)
{
	const string display = param.name.empty()
		? "#tt" + std::to_string(index) : param.name;
	TemplateInfo* info = unit_.templates.Create(TMPL_CLASS, display,
	                                            model_.global());
	info->tt_param_index = (int)index;
	info->params = param.tt_params;
	NamedTypeInfo* anchor = model_.CreateNamedTypeInfo(
		"template-parameter " + display, model_.global(), display);
	anchor->is_template_anchor = true;
	anchor->param_index = (int)index;
	anchor->spec_template = info;
	info->anchor = anchor;
	return info;
}

vector<TemplateArg> SemBinder::ResolveTemplateArgumentList(
	TemplateInfo& tmpl, const AstNamePart& part)
{
	const std::vector<AstTemplateArgument>& source = part.arguments;
	size_t pack_at = TemplatePackIndex(tmpl.params);
	bool has_pack = pack_at < tmpl.params.size();
	// The binding scope of the earlier parameters: created eagerly
	// when any parameter could need it (dependent value-parameter
	// types, defaults, packs); aliases bind incrementally as each
	// parameter resolves.
	bool needs_scope = has_pack || source.size() < tmpl.params.size();
	for (size_t i = 0; i < tmpl.params.size() && !needs_scope; i++)
		if (tmpl.params[i].kind == TPK_VALUE)
			needs_scope = true;
	Scope* partial = 0;
	vector<TemplateArg> args;
	if (needs_scope)
		EnsureArgBindingScope(tmpl, args, partial);
	// Source arguments fill the leading parameters one-to-one; once
	// the pack parameter is reached it absorbs every remaining source
	// argument (post-pack parameters can only be defaulted).
	size_t cursor = 0;
	for (size_t s = 0; s < source.size(); s++)
	{
		const AstTemplateArgument& argument = source[s];
		bool is_expansion = argument.pack ||
			(argument.is_type && argument.type &&
			 TypeIdHasPackMarker(*argument.type));
		if (cursor >= tmpl.params.size())
		{
			// PA21: a trailing expansion past the parameter list is
			// fine when it expands to nothing (`Alloc<U, Args...>`
			// with an empty Args against a fixed-arity template).
			if (is_expansion)
			{
				vector<TemplateArg> extra;
				TemplateParam dummy;
				ExpandTemplateArgumentPack(tmpl, dummy, argument,
				                           extra, partial);
				if (extra.empty())
					continue;
			}
			throw runtime_error("too many template arguments for " +
			                    tmpl.name);
		}
		const TemplateParam& param = tmpl.params[cursor];
		if (is_expansion)
		{
			if (!param.pack)
			{
				// 14.3p1 over a concrete expansion: the expanded
				// elements fill successive fixed parameters (a
				// pattern slot cannot).
				vector<TemplateArg> expanded;
				ExpandTemplateArgumentPack(tmpl, param, argument,
				                           expanded, partial);
				for (size_t k = 0; k < expanded.size(); k++)
				{
					if (cursor >= tmpl.params.size())
						throw runtime_error("too many template "
						                    "arguments for " +
						                    tmpl.name);
					if (expanded[k].pack_pattern)
						throw OutsideBoundary(
							"pack-expansion argument for a non-pack "
							"parameter");
					args.push_back(expanded[k]);
					if (!tmpl.params[cursor].pack)
					{
						if (partial)
							BindParamAlias(*partial,
							               tmpl.params[cursor],
							               args.back());
						cursor++;
					}
				}
				continue;
			}
			ExpandTemplateArgumentPack(tmpl, param, argument, args,
			                           partial);
		}
		else
			args.push_back(ResolveOneArgument(tmpl, param, argument,
			                                  args, partial));
		if (!param.pack)
		{
			if (partial)
				BindParamAlias(*partial, param, args.back());
			cursor++;
		}
	}
	// The pack's alias (possibly empty) binds once its run is known.
	if (has_pack && cursor == pack_at)
	{
		if (partial)
			BindPackAlias(*partial, tmpl.params[pack_at], args,
			              pack_at > args.size() ? args.size() : pack_at,
			              args.size());
		cursor = pack_at + 1;
	}
	FillDefaultedTail(tmpl, cursor, args, partial);
	return args;
}

// Defaults fill the remaining parameters of one resolved argument
// list (an unreached pack is empty).
void SemBinder::FillDefaultedTail(TemplateInfo& tmpl, size_t cursor,
                                  vector<TemplateArg>& args,
                                  Scope*& partial)
{
	for (; cursor < tmpl.params.size(); cursor++)
	{
		const TemplateParam& param = tmpl.params[cursor];
		if (param.pack)
		{
			if (partial)
				BindPackAlias(*partial, param, args, args.size(),
				              args.size());
			continue;
		}
		EnsureArgBindingScope(tmpl, args, partial);
		TemplateArg arg;
		Scope* saved = current_;
		current_ = partial;
		try
		{
			if (param.kind == TPK_TEMPLATE)
			{
				if (!param.default_type)
					throw runtime_error("too few template arguments "
					                    "for " + tmpl.name);
				const AstName* name = BareTypeIdName(*param.default_type);
				if (!name)
					throw runtime_error("defaulted template-template "
					                    "argument of " + tmpl.name +
					                    " does not name a template");
				arg = ResolveTemplateTemplateArgument(*name, param,
				                                      tmpl.name);
			}
			else if (param.kind == TPK_TYPE)
			{
				if (!param.default_type)
					throw runtime_error("too few template arguments "
					                    "for " + tmpl.name);
				arg = TemplateArg(builder_.ResolveTypeId(
					*param.default_type));
			}
			else
			{
				if (!param.default_expr)
					throw runtime_error("too few template arguments "
					                    "for " + tmpl.name);
				arg = ResolveDefaultValueExpr(
					*param.default_expr, ValueParamType(param, partial));
			}
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		args.push_back(arg);
		if (partial)
			BindParamAlias(*partial, param, args.back());
	}
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

// PA20: the full engine evaluates a converted constant expression
// (5.19p3) toward an integral/enum template-parameter type. Class
// operands convert through their (constexpr) conversion functions;
// non-constexpr conversions fail, which 14.3.2 requires.
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
			// PA20 14.3.2/5.19: only a constexpr conversion function
			// produces a converted constant expression.
			if (!DeclHasConstexpr(*conv.decl))
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
