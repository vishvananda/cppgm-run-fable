#include "sema/sem_binder.h"

#include <stdexcept>

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

// 14.8.2.5: structural unification of one parameter pattern against
// one argument type. `bound` has one slot per template parameter.
bool DeduceFromType(const TypePtr& pattern, const TypePtr& arg,
                    vector<TypePtr>& bound)
{
	if (!pattern || !arg)
		return false;
	if (pattern->kind == TK_TYPE_PARAM)
	{
		int index = pattern->named->param_index;
		if (index < 0 || (size_t)index >= bound.size())
			return false;
		TypePtr deduced = StripPatternCv(pattern, arg);
		if (bound[index])
			return TypeEquals(bound[index], deduced);
		bound[index] = deduced;
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
		// Match a specialization of the same template, walking the
		// single-inheritance chain for the derived-to-base case.
		const NamedTypeInfo* entity =
			arg->kind == TK_CLASS ? arg->named : 0;
		while (entity &&
		       entity->spec_template != pattern->named->spec_template)
			entity = entity->base_entity;
		if (!entity ||
		    entity->spec_args.size() != pattern->parameters.size())
			return false;
		for (size_t i = 0; i < pattern->parameters.size(); i++)
			if (!DeduceFromType(pattern->parameters[i],
			                    entity->spec_args[i], bound))
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
		binding.kind = SB_TYPE;
		binding.name = params[i].name;
		binding.type = PlaceholderType(i);
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
	return true;
}

// --- deduction ---------------------------------------------------------------

const FunctionSpecialization* SemBinder::DeduceFunctionTemplate(
	TemplateInfo& tmpl, const vector<SemValue>& args)
{
	EnsureFunctionPattern(tmpl);
	if (tmpl.param_patterns.size() < args.size())
		return 0;
	vector<TypePtr> bound(tmpl.params.size());
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
				if (bound[index] &&
				    !TypeEquals(bound[index], as_ref))
					return 0;
				bound[index] = as_ref;
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
		if (bound[i])
			continue;
		if (!tmpl.params[i].default_type)
			return 0;
		Scope* partial = MakeArgumentAliasScope(tmpl, bound);
		Scope* saved = current_;
		current_ = partial;
		try
		{
			bound[i] = builder_.ResolveTypeId(
				*tmpl.params[i].default_type);
		}
		catch (const std::exception&)
		{
			current_ = saved;
			return 0;
		}
		current_ = saved;
	}
	try
	{
		return EnsureFunctionSpecialization(tmpl, bound);
	}
	catch (const std::exception&)
	{
		// Substitution failure is a hard error in general, but a
		// candidate that cannot even form drops out here.
		return 0;
	}
}

// --- specialization -----------------------------------------------------------

FunctionSpecialization* SemBinder::EnsureFunctionSpecialization(
	TemplateInfo& tmpl, const vector<TypePtr>& args)
{
	if (args.size() != tmpl.params.size())
		throw runtime_error("wrong template argument count for " +
		                    tmpl.name);
	string key = TemplateArgumentKey(args);
	unique_ptr<FunctionSpecialization>& slot = tmpl.fn_specs[key];
	if (slot)
		return slot.get();
	if (instantiation_depth_ >= 200)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	slot.reset(new FunctionSpecialization());
	FunctionSpecialization* spec = slot.get();
	spec->owner = &tmpl;
	spec->args = args;
	spec->key = key;
	spec->name = tmpl.name + TemplateArgumentSpelling(args);
	spec->param_scope = MakeArgumentAliasScope(tmpl, args);

	// Compose the concrete signature in the template's context.
	const AstDecl& inner = *tmpl.pattern_decl;
	const AstDeclarator* declarator = inner.kind == DK_FUNCTION
		? inner.declarator.get()
		: inner.declarators[0].declarator.get();
	InstantiationContext context(*this, spec->param_scope);
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

	if (tmpl.has_definition)
		InstantiateFunctionBody(tmpl, *spec);
	return spec;
}

void SemBinder::InstantiateFunctionBody(TemplateInfo& tmpl,
                                        FunctionSpecialization& spec)
{
	if (spec.body_emitted)
		return;
	spec.body_emitted = true;
	const AstDecl& inner = *tmpl.pattern_decl;
	if (inner.kind != DK_FUNCTION || !inner.body)
		throw runtime_error("function template " + tmpl.name +
		                    " has no definition");
	InstantiationContext context(*this, spec.param_scope);
	bool saved_instantiating = instantiating_;
	instantiating_ = true;
	try
	{
		// Re-compose in this specialization's context so the parameter
		// names bind (positional identity with the declaring pattern).
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(inner.specifiers, true);
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			inner.declarator.get(), specs.type);
		Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, spec.name,
		                                     spec.param_scope);
		for (size_t i = 0; i < composed.parameters.size(); i++)
		{
			const ParameterInfo& parameter = composed.parameters[i];
			if (parameter.name.empty())
				continue;
			ScopeBinding binding;
			binding.kind = SB_PARAMETER;
			binding.name = parameter.name;
			binding.type = parameter.type;
			AddBinding(*fn_scope, binding);
		}

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
	catch (...)
	{
		instantiating_ = saved_instantiating;
		throw;
	}
	instantiating_ = saved_instantiating;
}

void SemBinder::InstantiatePendingFunctions(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<FunctionSpecialization>>::iterator it =
	         tmpl.fn_specs.begin();
	     it != tmpl.fn_specs.end(); ++it)
		if (!it->second->body_emitted)
			InstantiateFunctionBody(tmpl, *it->second);
}

// --- explicit forms ------------------------------------------------------------

const ScopeBinding* SemBinder::ResolveFunctionTemplateId(
	const ScopeBinding& binding, const AstNamePart& part)
{
	// Explicit arguments select among the templates of this name; the
	// PA18 subset requires the argument list (with defaults) to bind
	// every parameter.
	const FunctionSpecialization* resolved = 0;
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		TemplateInfo& tmpl = *binding.fn_templates[t];
		vector<TypePtr> args;
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
			if (!args[i] || TypeIsDependent(args[i]))
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
		throw runtime_error("no matching function template for " +
		                    part.identifier);
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
		ResolveFunctionTemplateId(*binding, terminal);
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
		vector<TypePtr> bound(tmpl.params.size());
		if (!DeduceFromType(tmpl.pattern, composed.type, bound))
			continue;
		bool complete = true;
		for (size_t i = 0; i < bound.size(); i++)
			if (!bound[i])
				complete = false;
		if (!complete)
			continue;
		EnsureFunctionSpecialization(tmpl, bound);
		return;
	}
	throw runtime_error("explicit instantiation matches no template");
}

Scope* SemBinder::SwapLookupScope(Scope* scope)
{
	Scope* previous = current_;
	current_ = scope;
	return previous;
}
