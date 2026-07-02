#include "sema/sem_binder.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA18 function templates: deduction patterns, direct-call argument
// deduction (14.8.2.1 subset), and on-demand instantiation of
// function-template specializations. Patterns compose over shared
// positional placeholder types (`#0`, `#1`, ...) so redeclarations
// with renamed parameters compare structurally; substitution composes
// the declarator again with the parameters aliased to the concrete
// types, so dependent forms (`typename T::type`) resolve through the
// ordinary machinery.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA18 assignment boundary");
}

// Removes exactly the cv-qualifiers the pattern spelled from the
// deduced type (`const T&` binding `const X` deduces `T = X`).
TypePtr StripPatternCv(const TypePtr& pattern, const TypePtr& deduced)
{
	if ((!pattern->is_const && !pattern->is_volatile) ||
	    IsReferenceType(deduced))
		return deduced;
	Type stripped = *deduced;
	if (pattern->is_const)
		stripped.is_const = false;
	if (pattern->is_volatile)
		stripped.is_volatile = false;
	return TypePtr(new Type(stripped));
}

// Whether a deduction slot has been bound (a type or a value).
bool ArgBound(const TemplateArg& arg)
{
	return arg.is_value || bool(arg.type);
}

bool DeduceFromType(const TypePtr& pattern, const TypePtr& arg,
                    vector<TemplateArg>& bound);

// Unification of one template-argument slot of a specialization
// pattern against the corresponding concrete argument. Value slots
// bind through the pattern's value-parameter reference or compare by
// value; type slots recurse.
bool DeduceFromArg(const TemplateArg& pattern, const TemplateArg& arg,
                   vector<TemplateArg>& bound)
{
	if (!pattern.is_value && !arg.is_value)
		return DeduceFromType(pattern.type, arg.type, bound);
	if (!pattern.is_value || !arg.is_value)
		return false;
	if (pattern.value_param >= 0)
	{
		if ((size_t)pattern.value_param >= bound.size())
			return false;
		TemplateArg& slot = bound[pattern.value_param];
		if (ArgBound(slot))
			return TemplateArgEquals(slot, arg);
		slot = arg;
		return true;
	}
	// A concrete value in the pattern must match exactly.
	return TemplateArgEquals(pattern, arg);
}

// 14.8.2.5: structural unification of one parameter pattern against
// one argument type. `bound` has one slot per template parameter.
bool DeduceFromType(const TypePtr& pattern, const TypePtr& arg,
                    vector<TemplateArg>& bound)
{
	if (!pattern || !arg)
		return false;
	if (pattern->kind == TK_TYPE_PARAM)
	{
		int index = pattern->named->param_index;
		if (index < 0 || (size_t)index >= bound.size())
			return false;
		TypePtr deduced = StripPatternCv(pattern, arg);
		if (ArgBound(bound[index]))
			return bound[index].type &&
				TypeEquals(bound[index].type, deduced);
		bound[index] = TemplateArg(deduced);
		return true;
	}
	if (pattern->kind != arg->kind)
	{
		// 14.8.2.1p4: a class-template parameter can deduce from a
		// derived class's unique matching base.
		if (pattern->kind == TK_TEMPLATE_SPEC && arg->kind == TK_CLASS)
			;  // handled below
		else
			return false;
	}
	switch (pattern->kind)
	{
	case TK_FUNDAMENTAL:
		return pattern->fundamental == arg->fundamental &&
			pattern->is_const == arg->is_const &&
			pattern->is_volatile == arg->is_volatile;
	case TK_CLASS:
	case TK_ENUM:
		return pattern->named == arg->named &&
			pattern->is_const == arg->is_const &&
			pattern->is_volatile == arg->is_volatile;
	case TK_POINTER:
		if (pattern->is_const != arg->is_const ||
		    pattern->is_volatile != arg->is_volatile)
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_ARRAY:
		if (pattern->bound_known &&
		    (!arg->bound_known || pattern->bound != arg->bound))
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_FUNCTION:
	{
		if (pattern->parameters.size() != arg->parameters.size() ||
		    pattern->variadic != arg->variadic ||
		    pattern->ref_qual != arg->ref_qual)
			return false;
		for (size_t i = 0; i < pattern->parameters.size(); i++)
			if (!DeduceFromType(pattern->parameters[i],
			                    arg->parameters[i], bound))
				return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	}
	case TK_MEMBER_POINTER:
		if (pattern->named != arg->named)
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_TEMPLATE_SPEC:
	{
		// A pattern-shaped argument (partial ordering's transformed
		// parameter types) matches the same template structurally.
		if (arg->kind == TK_TEMPLATE_SPEC)
		{
			if (arg->named->spec_template !=
			        pattern->named->spec_template ||
			    arg->targs.size() != pattern->targs.size())
				return false;
			for (size_t i = 0; i < pattern->targs.size(); i++)
				if (!DeduceFromArg(pattern->targs[i], arg->targs[i],
				                   bound))
					return false;
			return true;
		}
		// Match a specialization of the same template, walking the
		// single-inheritance chain for the derived-to-base case.
		const NamedTypeInfo* entity =
			arg->kind == TK_CLASS ? arg->named : 0;
		while (entity &&
		       entity->spec_template != pattern->named->spec_template)
			entity = entity->base_entity;
		if (!entity || entity->spec_args.size() != pattern->targs.size())
			return false;
		for (size_t i = 0; i < pattern->targs.size(); i++)
			if (!DeduceFromArg(pattern->targs[i], entity->spec_args[i],
			                   bound))
				return false;
		return true;
	}
	default:
		return false;
	}
}

// The declared argument type of one call argument for deduction
// purposes (14.8.2.1p2): arrays and functions decay, top-level cv
// drops for by-value parameters.
TypePtr DecayForDeduction(const TypePtr& type)
{
	return AdjustParameterType(type);
}

// 14.5.6.2p3: the transformed parameter type of one partial-ordering
// candidate - every type parameter replaced by its synthesized unique
// type. Null when the pattern has a shape outside the ordering subset.
TypePtr SubstituteOrderingTypes(const TypePtr& pattern,
                                const vector<TypePtr>& uniques)
{
	if (!pattern)
		return pattern;
	if (!TypeIsDependent(pattern))
		return pattern;
	switch (pattern->kind)
	{
	case TK_TYPE_PARAM:
	{
		int index = pattern->named->param_index;
		if (index < 0 || (size_t)index >= uniques.size())
			return TypePtr();
		return MakeCvQualifiedType(uniques[index], pattern->is_const,
		                           pattern->is_volatile);
	}
	case TK_POINTER:
	{
		TypePtr target = SubstituteOrderingTypes(pattern->target,
		                                         uniques);
		if (!target)
			return TypePtr();
		return MakePointerType(target, pattern->is_const,
		                       pattern->is_volatile);
	}
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	{
		TypePtr target = SubstituteOrderingTypes(pattern->target,
		                                         uniques);
		if (!target)
			return TypePtr();
		return MakeReferenceType(target,
		                         pattern->kind == TK_RVALUE_REFERENCE,
		                         false);
	}
	case TK_TEMPLATE_SPEC:
	{
		Type copy = *pattern;
		for (size_t i = 0; i < copy.targs.size(); i++)
		{
			if (copy.targs[i].is_value)
				continue;  // value slots compare by identity
			copy.targs[i].type = SubstituteOrderingTypes(
				copy.targs[i].type, uniques);
			if (!copy.targs[i].type)
				return TypePtr();
		}
		return TypePtr(new Type(copy));
	}
	default:
		return TypePtr();
	}
}

// The parameter clause of a function declarator (the first DI_PARAMS
// item, through one nesting level).
const AstParameterClause* FunctionParameterClause(
	const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return item.params.get();
		if (item.kind == DI_NESTED && item.nested)
			if (const AstParameterClause* inner =
			        FunctionParameterClause(*item.nested))
				return inner;
	}
	return 0;
}

}  // namespace

TypePtr SemBinder::PlaceholderType(size_t index)
{
	while (placeholders_.size() <= index)
	{
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			"typename #" + std::to_string(placeholders_.size()),
			model_.global(), "#" + std::to_string(placeholders_.size()));
		info->param_index = (int)placeholders_.size();
		placeholders_.push_back(MakeNamedType(TK_TYPE_PARAM, info));
	}
	return placeholders_[index];
}

TypePtr SemBinder::OrderingUniqueType(size_t index)
{
	while (ordering_uniques_.size() <= index)
	{
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			"struct #unique" + std::to_string(ordering_uniques_.size()),
			model_.global(),
			"#unique" + std::to_string(ordering_uniques_.size()));
		ordering_uniques_.push_back(MakeNamedType(TK_CLASS, info));
	}
	return ordering_uniques_[index];
}

// 14.5.6.2p8 (subset): `a` is at least as specialized as `b` for the
// leading `argc` parameters when `b`'s parameters deduce from `a`'s
// transformed parameter types.
bool SemBinder::OrderingAtLeastAsSpecialized(TemplateInfo& a,
                                             TemplateInfo& b,
                                             size_t argc)
{
	EnsureFunctionPattern(a);
	EnsureFunctionPattern(b);
	if (a.param_patterns.size() < argc || b.param_patterns.size() < argc)
		return false;
	vector<TypePtr> uniques;
	for (size_t i = 0; i < a.params.size(); i++)
		uniques.push_back(OrderingUniqueType(i));
	vector<TemplateArg> bound(b.params.size());
	for (size_t i = 0; i < argc; i++)
	{
		TypePtr transformed = SubstituteOrderingTypes(
			a.param_patterns[i], uniques);
		TypePtr pattern = b.param_patterns[i];
		if (!transformed || !pattern)
			return false;
		// 14.8.2.4p5-7: references and top-level cv-qualifiers drop
		// on both sides before deduction.
		if (IsReferenceType(pattern))
			pattern = pattern->target;
		if (IsReferenceType(transformed))
			transformed = transformed->target;
		pattern = RemoveTopCv(pattern);
		transformed = RemoveTopCv(transformed);
		if (!TypeIsDependent(pattern))
		{
			if (!TypeEquals(pattern, transformed))
				return false;
			continue;
		}
		if (!DeduceFromType(pattern, transformed, bound))
			return false;
	}
	return true;
}

bool SemBinder::TemplateCandidateMoreSpecialized(
	const FunctionSpecialization* a, const FunctionSpecialization* b,
	size_t argc)
{
	if (!a || !b || !a->owner || !b->owner || a->owner == b->owner)
		return false;
	return OrderingAtLeastAsSpecialized(*a->owner, *b->owner, argc) &&
		!OrderingAtLeastAsSpecialized(*b->owner, *a->owner, argc);
}

// Composes the declarator of a function-template declaration with the
// parameters bound to the positional placeholders. Returns false when
// the signature does not compose abstractly (dependent qualified
// forms); per-parameter patterns then compose individually.
bool SemBinder::ComposeFunctionPattern(
	const vector<TemplateParam>& params, Scope* declaring,
	const AstDecl& inner, TypePtr& full, vector<TypePtr>& param_patterns)
{
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                  declaring);
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].name.empty())
			continue;
		ScopeBinding binding;
		binding.name = params[i].name;
		if (params[i].kind == TPK_TYPE)
		{
			binding.kind = SB_TYPE;
			binding.type = PlaceholderType(i);
		}
		else
		{
			// An abstract value parameter: uses of the name inside the
			// pattern signature become value-parameter slots.
			binding.kind = SB_VARIABLE;
			binding.no_object = true;
			binding.param_index = (int)i;
		}
		AddBinding(*scope, binding);
	}
	const AstDeclarator* declarator = inner.kind == DK_FUNCTION
		? inner.declarator.get()
		: inner.declarators[0].declarator.get();
	Scope* saved = current_;
	current_ = scope;
	bool composed_full = false;
	full = TypePtr();
	param_patterns.clear();
	try
	{
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(inner.specifiers, true);
		DeclaratorInfo composed =
			builder_.ComposeDeclarator(declarator, specs.type);
		if (composed.declares_function &&
		    composed.type->kind == TK_FUNCTION)
		{
			full = composed.type;
			for (size_t i = 0; i < composed.parameters.size(); i++)
				param_patterns.push_back(composed.parameters[i].type);
			composed_full = true;
		}
	}
	catch (const std::exception&)
	{
		// Dependent forms in the signature: fall through to the
		// per-parameter pass.
	}
	if (!composed_full && declarator)
	{
		// Compose each parameter's declared type individually; a
		// parameter that still fails is a non-deduced context.
		const AstParameterClause* clause = FunctionParameterClause(
			*declarator);
		if (clause)
			for (size_t i = 0; i < clause->parameters.size(); i++)
			{
				TypePtr pattern;
				try
				{
					DeclSpecifierInfo pspecs = builder_.ProcessSpecifiers(
						clause->parameters[i].specifiers, false);
					DeclaratorInfo pcomposed = builder_.ComposeDeclarator(
						clause->parameters[i].declarator.get(),
						pspecs.type);
					pattern = pcomposed.type;
				}
				catch (const std::exception&)
				{
					pattern = TypePtr();
				}
				param_patterns.push_back(pattern);
			}
	}
	current_ = saved;
	return composed_full;
}

void SemBinder::EnsureFunctionPattern(TemplateInfo& tmpl)
{
	if (tmpl.pattern_ready)
		return;
	tmpl.pattern_ready = true;
	ComposeFunctionPattern(tmpl.params, tmpl.declaring,
	                       *tmpl.pattern_decl, tmpl.pattern,
	                       tmpl.param_patterns);
}

bool SemBinder::SameFunctionTemplateSignature(TemplateInfo& tmpl,
                                              const AstDecl& decl,
                                              const AstDecl& inner)
{
	EnsureFunctionPattern(tmpl);
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	TypePtr full;
	vector<TypePtr> param_patterns;
	bool composed = ComposeFunctionPattern(params, tmpl.declaring, inner,
	                                       full, param_patterns);
	if (composed != bool(tmpl.pattern))
		return false;
	if (composed)
		return TypeEquals(full, tmpl.pattern);
	// Neither signature composes abstractly: compare the composable
	// parameter patterns positionally as a conservative identity.
	if (param_patterns.size() != tmpl.param_patterns.size())
		return false;
	for (size_t i = 0; i < param_patterns.size(); i++)
	{
		if (bool(param_patterns[i]) != bool(tmpl.param_patterns[i]))
			return false;
		if (param_patterns[i] &&
		    !TypeEquals(param_patterns[i], tmpl.param_patterns[i]))
			return false;
	}
	// The dependent return spelling distinguishes overloads the
	// abstract composition cannot see (`typename T::A f(T)` vs
	// `typename T::B f(T)` are distinct templates).
	if (PositionalizeTemplateNames(FlattenSpecifierSeq(inner.specifiers),
	                               params) !=
	    PositionalizeTemplateNames(
	        FlattenSpecifierSeq(tmpl.pattern_decl->specifiers),
	        tmpl.params))
		return false;
	return true;
}

// --- deduction ---------------------------------------------------------------

const FunctionSpecialization* SemBinder::DeduceFunctionTemplate(
	TemplateInfo& tmpl, const vector<SemValue>& args,
	const AstNamePart* explicit_part)
{
	EnsureFunctionPattern(tmpl);
	if (tmpl.param_patterns.size() < args.size())
		return 0;
	vector<TemplateArg> bound(tmpl.params.size());
	// 14.8.1: explicit template arguments bind the leading parameters.
	if (explicit_part)
	{
		if (explicit_part->arguments.size() > tmpl.params.size())
			return 0;
		for (size_t i = 0; i < explicit_part->arguments.size(); i++)
		{
			const AstTemplateArgument& argument =
				explicit_part->arguments[i];
			if (argument.pack)
				return 0;
			const TemplateParam& param = tmpl.params[i];
			try
			{
				if (param.kind == TPK_TYPE)
				{
					if (!argument.is_type || !argument.type)
						return 0;
					bound[i] = TemplateArg(
						builder_.ResolveTypeId(*argument.type));
				}
				else
				{
					Scope* partial =
						MakeArgumentAliasScope(tmpl, bound);
					bound[i] = ResolveValueArgument(
						argument, ValueParamType(param, partial));
				}
			}
			catch (const std::exception&)
			{
				return 0;
			}
			if (TemplateArgIsDependent(bound[i]))
				return 0;
		}
	}
	for (size_t i = 0; i < args.size(); i++)
	{
		const TypePtr& pattern = tmpl.param_patterns[i];
		if (!pattern)
			continue;  // non-deduced context
		if (!TypeIsDependent(pattern))
			continue;  // ordinary conversion checking applies later
		TypePtr arg_type = args[i].type;
		if (!arg_type)
			return 0;
		if (IsReferenceType(pattern))
		{
			TypePtr referee = pattern->target;
			// 14.8.2.1p3: a forwarding reference binding an lvalue
			// deduces the parameter as an lvalue reference.
			if (pattern->kind == TK_RVALUE_REFERENCE &&
			    referee->kind == TK_TYPE_PARAM && !referee->is_const &&
			    !referee->is_volatile &&
			    args[i].category == VC_LVALUE)
			{
				int index = referee->named->param_index;
				if (index < 0 || (size_t)index >= bound.size())
					return 0;
				TypePtr as_ref =
					MakeReferenceType(arg_type, false, true);
				if (ArgBound(bound[index]) &&
				    !(bound[index].type &&
				      TypeEquals(bound[index].type, as_ref)))
					return 0;
				bound[index] = TemplateArg(as_ref);
				continue;
			}
			if (!DeduceFromType(referee, arg_type, bound))
				return 0;
			continue;
		}
		if (!DeduceFromType(RemoveTopCv(pattern),
		                    DecayForDeduction(arg_type), bound))
			return 0;
	}
	// Unbound parameters fill from default template arguments; any
	// remaining hole is a deduction failure.
	for (size_t i = 0; i < bound.size(); i++)
	{
		if (ArgBound(bound[i]))
			continue;
		const TemplateParam& param = tmpl.params[i];
		if (!param.default_type && !param.default_expr)
			return 0;
		Scope* partial = MakeArgumentAliasScope(tmpl, bound);
		Scope* saved = current_;
		current_ = partial;
		try
		{
			if (param.kind == TPK_TYPE)
				bound[i] = TemplateArg(builder_.ResolveTypeId(
					*param.default_type));
			else
				bound[i] = ResolveDefaultValueExpr(
					*param.default_expr,
					ValueParamType(param, partial));
		}
		catch (const std::exception&)
		{
			current_ = saved;
			return 0;
		}
		current_ = saved;
	}
	// Substitution failure is a hard error (SFINAE candidate dropping
	// is out of scope); the body instantiates only on odr-use.
	return EnsureFunctionSpecialization(tmpl, bound);
}

// --- specialization -----------------------------------------------------------

FunctionSpecialization* SemBinder::EnsureFunctionSpecialization(
	TemplateInfo& tmpl, const vector<TemplateArg>& args)
{
	if (args.size() != tmpl.params.size())
		throw runtime_error("wrong template argument count for " +
		                    tmpl.name);
	string key = TemplateArgumentKey(args);
	{
		map<string, unique_ptr<FunctionSpecialization>>::iterator found =
			tmpl.fn_specs.find(key);
		if (found != tmpl.fn_specs.end())
			return found->second.get();
	}
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	// The record enters the cache only after the signature composes:
	// a substitution failure must not leave a half-built entry behind.
	unique_ptr<FunctionSpecialization> fresh(new FunctionSpecialization());
	FunctionSpecialization* spec = fresh.get();
	spec->owner = &tmpl;
	spec->args = args;
	spec->key = key;
	spec->name = tmpl.name + TemplateArgumentSpelling(args);
	spec->param_scope = MakeArgumentAliasScope(tmpl, args);

	// Compose the concrete signature in the template's context; the
	// parameters bind into a scratch scope so a trailing-return
	// decltype can name them (8.3.5p2).
	const AstDecl& inner = *tmpl.pattern_decl;
	const AstDeclarator* declarator = inner.kind == DK_FUNCTION
		? inner.declarator.get()
		: inner.declarators[0].declarator.get();
	Scope* capture = model_.CreateScope(SCOPE_FUNCTION, tmpl.name,
	                                    spec->param_scope);
	InstantiationContext context(*this, capture);
	param_capture_scope_ = capture;
	PreBindDeclaredParameters(declarator);
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(declarator, specs.type);
	if (!composed.declares_function ||
	    composed.type->kind != TK_FUNCTION)
		throw runtime_error("function template " + tmpl.name +
		                    " does not declare a function");
	spec->type = composed.type;
	for (size_t i = 0; i < composed.parameters.size(); i++)
		spec->declared_params.push_back(composed.parameters[i].type);

	spec->self.kind = SB_FUNCTION;
	spec->self.name = spec->name;
	spec->self.type = composed.type;
	spec->self.fn_self_spec = spec;
	// The alias scope is the specialization's identity scope: the
	// lowering keys the definition and its call sites on it, and the
	// canonical-name walk skips it up to the declaring namespace.
	spec->self.owner = spec->param_scope;
	spec->self.home = spec->param_scope;
	spec->self.fn_defaults.resize(1);
	spec->self.fn_deleted.resize(1, false);
	spec->self.fn_access.resize(1, MA_PUBLIC);
	spec->self.fn_static.resize(1, false);
	spec->self.fn_inline_def.resize(1, false);
	spec->self.fn_adl_only.resize(1, false);
	spec->self.fn_unwind_no.resize(1, composed.noexcept_simple);
	spec->self.fn_owner.resize(1, spec->param_scope);
	vector<const AstExpr*>& defaults = spec->self.fn_defaults[0];
	defaults.resize(composed.parameters.size(), 0);
	for (size_t i = 0; i < composed.parameters.size(); i++)
		defaults[i] = composed.parameters[i].default_arg;

	tmpl.fn_specs[key] = std::move(fresh);
	return spec;
}

// 14.7.1p2: the first odr-use of a deduced specialization
// instantiates its body; a use before the definition re-checks when
// the definition is captured, and the end-of-unit pass errors on
// odr-used specializations that never gained one.
void SemBinder::OnSpecializationOdrUsed(const FunctionSpecialization* spec)
{
	// 3.2p2: names in unevaluated operands are not odr-used.
	if (!spec || in_unevaluated_operand_)
		return;
	FunctionSpecialization& used =
		*const_cast<FunctionSpecialization*>(spec);
	used.odr_used = true;
	if (used.owner && used.owner->has_definition && !used.body_emitted)
		InstantiateFunctionBody(*used.owner, used);
}

void SemBinder::InstantiateFunctionBody(TemplateInfo& tmpl,
                                        FunctionSpecialization& spec)
{
	if (spec.body_emitted)
		return;
	// Set before the bind as the recursion guard (a recursive call in
	// the body finds its own specialization already in progress); a
	// bind failure propagates as a hard error, so the flag never
	// outlives a failed instantiation.
	spec.body_emitted = true;
	const AstDecl& inner = *tmpl.pattern_decl;
	if (inner.kind != DK_FUNCTION || !inner.body)
		throw runtime_error("function template " + tmpl.name +
		                    " has no definition");
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, spec.name,
	                                     spec.param_scope);
	fn_scope->fn_type = spec.type;
	InstantiationContext context(*this, fn_scope, true);
	param_capture_scope_ = fn_scope;
	// Pre-bind the parameters so the trailing-return decltype (which
	// composes before the clause, 8.3.5p2) can name them, then
	// re-compose the declarator in this specialization's context.
	PreBindDeclaredParameters(inner.declarator.get());
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		inner.declarator.get(), specs.type);

	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	SemNode* node = item.get();
	item->name = CanonicalQualifiedName(tmpl.declaring, spec.name);
	item->type = composed.type;
	item->entity_scope = spec.param_scope;
	item->entity_name = spec.name;
	item->unwind_no = composed.noexcept_simple;
	item->inline_def = true;
	item->fn_spec = &spec;
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = composed.parameters[i].name;
		parameter->type = composed.parameters[i].type;
		parameter->entity_scope = fn_scope;
		parameter->entity_name = parameter->name;
		AttachParameterDtor(*parameter);
		item->children.push_back(std::move(parameter));
	}
	current_ = fn_scope;
	method_.fn_scope = fn_scope;
	method_.fn_owner = tmpl.declaring;
	method_.fn_name = spec.name;
	current_return_ = composed.type->target;
	parents_.push_back(node);
	BindStatement(*inner.body);
	parents_.pop_back();

	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	if (!may_throw)
	{
		node->unwind_no = true;
		spec.self.fn_unwind_no[0] = true;
	}
	unit_.deferred.push_back(std::move(item));
}

void SemBinder::PreBindDeclaredParameters(const AstDeclarator* declarator)
{
	if (!declarator)
		return;
	const AstParameterClause* clause = FunctionParameterClause(*declarator);
	if (!clause)
		return;
	for (size_t i = 0; i < clause->parameters.size(); i++)
	{
		try
		{
			DeclSpecifierInfo pspecs = builder_.ProcessSpecifiers(
				clause->parameters[i].specifiers, false);
			DeclaratorInfo pcomposed = builder_.ComposeDeclarator(
				clause->parameters[i].declarator.get(), pspecs.type);
			if (pcomposed.id && pcomposed.id->IsPlainIdentifier())
				OnParameterComposed(pcomposed.id->parts[0].identifier,
				                    pcomposed.type);
		}
		catch (const std::exception&)
		{
			// The full composition reports real errors.
		}
	}
}

void SemBinder::InstantiatePendingFunctions(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<FunctionSpecialization>>::iterator it =
	         tmpl.fn_specs.begin();
	     it != tmpl.fn_specs.end(); ++it)
		if (it->second->odr_used && !it->second->body_emitted)
			InstantiateFunctionBody(tmpl, *it->second);
}

// --- explicit forms ------------------------------------------------------------

const ScopeBinding* SemBinder::ResolveFunctionTemplateId(
	const ScopeBinding& binding, const AstNamePart& part)
{
	// Explicit arguments select among the templates of this name; the
	// PA18 subset requires the argument list (with defaults) to bind
	// every parameter. An argument list that does not bind them all is
	// not an error here: a partial-explicit template-id falls back to
	// the overload set so the call context deduces the rest (14.8.1).
	const FunctionSpecialization* resolved = 0;
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		TemplateInfo& tmpl = *binding.fn_templates[t];
		vector<TemplateArg> args;
		try
		{
			args = ResolveTemplateArgumentList(tmpl, part);
		}
		catch (const std::exception&)
		{
			continue;
		}
		bool dependent = false;
		for (size_t i = 0; i < args.size(); i++)
			if ((!args[i].is_value && !args[i].type) ||
			    TemplateArgIsDependent(args[i]))
				dependent = true;
		if (dependent)
			continue;
		const FunctionSpecialization* spec =
			EnsureFunctionSpecialization(tmpl, args);
		if (resolved && resolved != spec)
			throw runtime_error("ambiguous template-id " +
			                    part.identifier);
		resolved = spec;
	}
	if (!resolved)
		// A call context can still deduce the remaining parameters
		// from the arguments (14.8.1); hand back the overload set.
		return &binding;
	return &resolved->self;
}

void SemBinder::BindExplicitFunctionInstantiation(const AstDecl& inner)
{
	// `template void f<int>(int);` / `template void f(int);`: resolve
	// the declarator against the visible function templates.
	if (inner.declarators.size() != 1 ||
	    !inner.declarators[0].declarator)
		throw OutsideBoundary("explicit instantiation declarator");
	const AstName* id = inner.declarators[0].declarator->IdName();
	if (!id || id->parts.empty())
		throw OutsideBoundary("explicit instantiation declarator");
	const AstNamePart& terminal = id->parts.back();
	if (terminal.kind != NP_IDENTIFIER &&
	    terminal.kind != NP_TEMPLATE_ID)
		throw OutsideBoundary("explicit instantiation name form");
	// Resolve the (possibly qualified) name to its function binding.
	Scope* prefix = 0;
	if (id->parts.size() > 1 || id->global_scope)
		prefix = ResolvePrefixScope(*id);
	const ScopeBinding* binding = prefix
		? QualifiedLookup(*prefix, terminal.identifier, SLF_ANY)
		: UnqualifiedLookup(current_, terminal.identifier, SLF_ANY);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("explicit instantiation of a non-function");
	if (terminal.kind == NP_TEMPLATE_ID)
	{
		// 14.7.2: the explicit instantiation demands the definition.
		const ScopeBinding* named =
			ResolveFunctionTemplateId(*binding, terminal);
		if (named && named->fn_self_spec)
			OnSpecializationOdrUsed(named->fn_self_spec);
		return;
	}
	// Deduce the template arguments from the declared parameter types.
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		inner.declarators[0].declarator.get(), specs.type);
	if (composed.type->kind != TK_FUNCTION)
		throw runtime_error("explicit instantiation of a non-function");
	for (size_t t = 0; t < binding->fn_templates.size(); t++)
	{
		TemplateInfo& tmpl = *binding->fn_templates[t];
		EnsureFunctionPattern(tmpl);
		if (!tmpl.pattern)
			continue;
		vector<TemplateArg> bound(tmpl.params.size());
		if (!DeduceFromType(tmpl.pattern, composed.type, bound))
			continue;
		bool complete = true;
		for (size_t i = 0; i < bound.size(); i++)
			if (!ArgBound(bound[i]))
				complete = false;
		if (!complete)
			continue;
		// 14.7.2: the explicit instantiation demands the definition.
		OnSpecializationOdrUsed(EnsureFunctionSpecialization(tmpl, bound));
		return;
	}
	throw runtime_error("explicit instantiation matches no template");
}

void SemBinder::OnParameterComposed(const string& name,
                                    const TypePtr& type)
{
	if (!param_capture_scope_ || name.empty())
		return;
	if (FindOwnBinding(*param_capture_scope_, name))
		return;
	ScopeBinding binding;
	binding.kind = SB_PARAMETER;
	binding.name = name;
	// 8.3.5p5: an array- or function-typed parameter declares the
	// adjusted pointer object.
	binding.type = type->kind == TK_ARRAY || type->kind == TK_FUNCTION
		? AdjustParameterType(type) : type;
	AddBinding(*param_capture_scope_, binding);
}

Scope* SemBinder::SwapLookupScope(Scope* scope)
{
	Scope* previous = current_;
	current_ = scope;
	return previous;
}

void SemBinder::RequireCompleteType(const NamedTypeInfo* info)
{
	EnsureTypeCompleteness(info);
}

const FunctionSpecialization* SemBinder::DeduceFunctionTemplateFromTarget(
	TemplateInfo& tmpl, const TypePtr& target)
{
	EnsureFunctionPattern(tmpl);
	if (!tmpl.pattern || !target || target->kind != TK_FUNCTION)
		return 0;
	vector<TemplateArg> bound(tmpl.params.size());
	if (!DeduceFromType(tmpl.pattern, target, bound))
		return 0;
	for (size_t i = 0; i < bound.size(); i++)
		if (!ArgBound(bound[i]))
			return 0;
	// Substitution failure is a hard error (no SFINAE dropping).
	return EnsureFunctionSpecialization(tmpl, bound);
}
