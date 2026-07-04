#include "sema/sem_binder.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA21 function-template bodies and explicit forms, split from
// template_deduce.cpp: on-demand body instantiation (14.7.1p2),
// explicit template-id resolution, and explicit instantiation of
// function templates and specialization members (14.7.2).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA18 assignment boundary");
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

// The instantiated body's parameter nodes. Slot names follow the
// first declaration (the primary pattern); unnamed definition
// parameters fall back to it, and an explicit definition's renamed
// parameters redirect onto the primary-named slots.
void SemBinder::AttachBodyParameters(TemplateInfo& tmpl,
                                     FunctionSpecialization& spec,
                                     const DeclaratorInfo& composed,
                                     Scope* fn_scope, SemNode& item)
{
	vector<string> slot_names(composed.parameters.size());
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		slot_names[i] = composed.parameters[i].name;
		if (slot_names[i].empty() &&
		    i < tmpl.declared_param_names.size())
			slot_names[i] = tmpl.declared_param_names[i];
	}
	if (spec.explicit_def && tmpl.pattern_decl &&
	    tmpl.pattern_decl->declarator)
		RedirectExplicitSlotNames(*tmpl.pattern_decl->declarator,
		                          fn_scope, slot_names);
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = slot_names[i];
		parameter->type = composed.parameters[i].type;
		parameter->entity_scope = fn_scope;
		parameter->entity_name = parameter->name;
		AttachParameterDtor(*parameter);
		item.children.push_back(std::move(parameter));
	}
}

// PA21: the implicit object parameter of an instantiated member
// -template body (the this-adjusted item type, the `this` slot, and
// its function-scope binding).
void SemBinder::AttachMemberBodyThis(const ClassInfo& cls,
                                     const TypePtr& declared,
                                     Scope* fn_scope, SemNode& item)
{
	item.type = MethodAdjustedType(cls, declared);
	SemNodePtr this_param = MakeSemNode(SN_PARAMETER);
	this_param->name = "this";
	this_param->type = item.type->parameters[0];
	this_param->entity_scope = fn_scope;
	this_param->entity_name = "this";
	item.children.push_back(std::move(this_param));
	if (!FindOwnBinding(*fn_scope, "this"))
	{
		ScopeBinding this_binding;
		this_binding.kind = SB_PARAMETER;
		this_binding.name = "this";
		this_binding.type = item.type->parameters[0];
		AddBinding(*fn_scope, this_binding);
	}
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

// The conversion-type-id of a conversion-function template pattern
// (null for other patterns).
static const AstTypeId* PatternConversionTypeId(const AstDecl& inner)
{
	if (inner.kind != DK_SPECIAL_MEMBER_DEFINITION || !inner.declarator)
		return 0;
	const AstName* id = inner.declarator->IdName();
	if (!id || id->parts.empty() ||
	    id->parts.back().kind != NP_CONVERSION_FUNCTION)
		return 0;
	return id->parts.back().conversion_type.get();
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
	// An explicit specialization's own definition replaces the primary
	// pattern (14.7.3); a use before it deduced only the signature.
	spec.body_emitted = true;
	const AstDecl& inner = spec.explicit_def ? *spec.explicit_def
	                                         : *tmpl.pattern_decl;
	// PA22 conversion-function templates: the pattern is a special
	// member whose result type is its conversion-type-id.
	const AstTypeId* conversion_type = PatternConversionTypeId(inner);
	if ((inner.kind != DK_FUNCTION && !conversion_type) || !inner.body)
		throw runtime_error("function template " + tmpl.name +
		                    " has no definition");
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, spec.name,
	                                     spec.param_scope);
	fn_scope->fn_type = spec.type;
	InstantiationContext context(*this, fn_scope, true);
	param_capture_scope_ = fn_scope;
	// PA21 member templates: the trailing-return decltype may name
	// the enclosing class's members through the implicit this.
	if (tmpl.member_of && !tmpl.member_static)
	{
		method_.cls = unit_.classes.Find(tmpl.member_of);
		if (method_.cls)
			method_.this_type = MakePointerType(
				MakeNamedType(TK_CLASS, tmpl.member_of), false, false);
	}
	// Pre-bind the parameters so the trailing-return decltype (which
	// composes before the clause, 8.3.5p2) can name them, then
	// re-compose the declarator in this specialization's context.
	PreBindDeclaredParameters(inner.declarator.get());
	last_pack_param_ = PackParamRecord();
	DeclSpecifierInfo specs;
	if (conversion_type)
		specs.type = builder_.ResolveTypeId(*conversion_type);
	else
		specs = builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		inner.declarator.get(), specs.type);
	BindCapturedPackParameter(fn_scope);

	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	SemNode* node = item.get();
	item->name = CanonicalQualifiedName(tmpl.declaring, spec.name);
	item->type = composed.type;
	item->entity_scope = spec.param_scope;
	item->entity_name = spec.name;
	item->unwind_no = composed.noexcept_simple;
	// An explicit specialization is an ordinary definition: strong
	// unless declared inline (7.1.2); instantiated bodies stay weak.
	item->inline_def = !spec.explicit_def || spec.explicit_inline;
	item->fn_spec = &spec;
	// 7.1.5: constexpr on the pattern (or explicit-specialization)
	// declaration makes the instantiated body engine-evaluable and
	// implicitly inline.
	if (DeclHasConstexpr(inner))
	{
		item->is_constexpr_fn = true;
		item->inline_def = true;
	}
	// PA21 member function templates: the body binds as a method (the
	// implicit object parameter first, the class as `this` context).
	const ClassInfo* member_cls = 0;
	if (tmpl.member_of && !tmpl.member_static)
		member_cls = unit_.classes.Find(tmpl.member_of);
	if (member_cls)
		AttachMemberBodyThis(*member_cls, composed.type, fn_scope,
		                     *item);
	AttachBodyParameters(tmpl, spec, composed, fn_scope, *item);
	current_ = fn_scope;
	method_.fn_scope = fn_scope;
	method_.fn_owner = tmpl.declaring;
	method_.fn_name = spec.name;
	method_.fn_template_name = tmpl.name;
	if (tmpl.member_of)
	{
		method_.cls = unit_.classes.Find(tmpl.member_of);
		if (member_cls)
			method_.this_type = item->type->parameters[0];
	}
	current_return_ = composed.type->target;
	parents_.push_back(node);
	try
	{
		BindStatement(*inner.body);
	}
	catch (...)
	{
		// A failed instantiation leaves no trace: a poisoning caller
		// (an eagerly bound sibling body) may retry later, and the
		// odr-use that triggered this bind dies with it.
		spec.body_emitted = false;
		spec.odr_used = false;
		throw;
	}
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
	if (item->inline_def)
		unit_.deferred.push_back(std::move(item));
	else
		unit_.items.push_back(std::move(item));
}

// An explicit specialization's renamed parameters redirect their
// body bindings onto the primary declaration's slot names (the
// lowering resolves slots by the primary spelling).
void SemBinder::RedirectExplicitSlotNames(const AstDeclarator& primary,
                                          Scope* fn_scope,
                                          vector<string>& slot_names)
{
	const AstParameterClause* primary_clause =
		FunctionParameterClause(primary);
	for (size_t i = 0;
	     primary_clause && i < primary_clause->parameters.size() &&
	     i < slot_names.size(); i++)
	{
		const AstDeclarator* pd =
			primary_clause->parameters[i].declarator.get();
		const AstName* pid = pd ? pd->IdName() : 0;
		if (!pid || !pid->IsPlainIdentifier())
			continue;
		const string& primary_name = pid->parts[0].identifier;
		if (primary_name.empty() || primary_name == slot_names[i])
			continue;
		if (!slot_names[i].empty())
			if (ScopeBinding* redirect =
			        FindOwnBinding(*fn_scope, slot_names[i]))
			{
				redirect->pack_element_name = primary_name;
				ScopeBinding slot_binding;
				slot_binding.kind = SB_PARAMETER;
				slot_binding.name = primary_name;
				slot_binding.type = redirect->type;
				AddBinding(*fn_scope, slot_binding);
			}
		slot_names[i] = primary_name;
	}
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
			// A pack parameter pre-binds its expanded pack so the
			// trailing-return decltype (which composes before the
			// clause, 8.3.5p2) can expand `args...`; the expansion
			// publishes the captured binding itself.
			vector<ParameterInfo> expanded;
			if (ExpandPackParameter(clause->parameters[i], expanded))
				continue;
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
		// A template with a parameter pack defers to the call context:
		// the pack may still gain elements from argument deduction
		// (14.8.1 with a trailing pack).
		if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
			continue;
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
		const FunctionSpecialization* spec;
		try
		{
			spec = EnsureFunctionSpecialization(tmpl, args);
		}
		catch (const InstantiationBodyFault&)
		{
			throw;
		}
		catch (const std::exception&)
		{
			// 14.8.2p8: substitution failure drops this template.
			continue;
		}
		if (resolved && resolved != spec)
			// 14.8.1: several templates accept the arguments; the use
			// context (call ranking, 13.4 target selection) picks.
			return &binding;
		resolved = spec;
	}
	if (!resolved)
		// A call context can still deduce the remaining parameters
		// from the arguments (14.8.1); hand back the overload set.
		return &binding;
	return &resolved->self;
}

void SemBinder::BindExplicitFunctionInstantiation(const AstDecl& inner,
                                                  bool is_extern)
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
	    terminal.kind != NP_TEMPLATE_ID &&
	    terminal.kind != NP_OPERATOR_FUNCTION)
		throw OutsideBoundary("explicit instantiation name form");
	const string terminal_name = terminal.kind == NP_TEMPLATE_ID
		? terminal.identifier : DeclaredFunctionName(terminal);
	// Resolve the (possibly qualified) name to its function binding.
	Scope* prefix = 0;
	if (id->parts.size() > 1 || id->global_scope)
		prefix = ResolvePrefixScope(*id);
	const ScopeBinding* binding = prefix
		? QualifiedLookup(*prefix, terminal_name, SLF_ANY)
		: UnqualifiedLookup(current_, terminal_name, SLF_ANY);
	// 14.7.2: a static data member of a specialization (`extern
	// template const int box<int>::n;`). Member definitions
	// instantiate on demand only, so the extern declaration needs no
	// suppression record; the definition form roots the member's
	// already-registered definition like a member function's.
	if (binding && binding->kind == SB_VARIABLE && prefix &&
	    prefix->kind == SCOPE_CLASS)
	{
		if (is_extern)
			return;
		SemUnit::ExplicitMemberInstantiation record;
		record.scope = prefix;
		record.name = terminal_name;
		unit_.explicit_member_instantiations.push_back(record);
		return;
	}
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("explicit instantiation of a non-function");
	// A member of a class-template specialization (`template int
	// tester<int>::test();`): the member's already-instantiated
	// definition becomes an unconditional emission root.
	if (binding->fn_templates.empty() && prefix &&
	    prefix->kind == SCOPE_CLASS)
	{
		if (is_extern)
			return;
		SemUnit::ExplicitMemberInstantiation record;
		record.scope = prefix;
		record.name = terminal_name;
		unit_.explicit_member_instantiations.push_back(record);
		return;
	}
	// The declared signature disambiguates same-name templates whose
	// explicit argument lists would both resolve.
	TypePtr declared;
	try
	{
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(inner.specifiers, true);
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			inner.declarators[0].declarator.get(), specs.type);
		if (composed.type->kind == TK_FUNCTION)
			declared = composed.type;
	}
	catch (const std::exception&)
	{
		declared = TypePtr();
	}
	const FunctionSpecialization* resolved = 0;
	bool has_args = terminal.kind == NP_TEMPLATE_ID ||
		!terminal.arguments.empty();
	if (has_args)
	{
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !resolved; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
				continue;
			vector<TemplateArg> args;
			bool have_args = true;
			try
			{
				args = ResolveTemplateArgumentList(tmpl, terminal);
			}
			catch (const std::exception&)
			{
				have_args = false;
			}
			FunctionSpecialization* spec = 0;
			if (have_args)
			{
				try
				{
					spec = EnsureFunctionSpecialization(tmpl, args);
				}
				catch (const std::exception&)
				{
					spec = 0;
				}
			}
			if (!spec)
			{
				// 14.7.2 with 14.8.2.2: arguments the template-id
				// omits (`f<>`) deduce from the declared signature;
				// the explicit prefix must agree with the deduction.
				if (!declared ||
				    terminal.arguments.size() >= tmpl.params.size())
					continue;
				const FunctionSpecialization* deduced =
					DeduceFunctionTemplateFromTarget(tmpl, declared);
				if (!deduced)
					continue;
				if (have_args)
				{
					if (deduced->args.size() < args.size())
						continue;
					vector<TemplateArg> lead(
						deduced->args.begin(),
						deduced->args.begin() + args.size());
					if (TemplateArgumentKey(lead) !=
					    TemplateArgumentKey(args))
						continue;
				}
				else if (!terminal.arguments.empty())
					continue;
				resolved = deduced;
				continue;
			}
			if (declared && !TypeEquals(spec->type, declared))
				continue;
			resolved = spec;
		}
	}
	else
	{
		if (!declared)
			throw runtime_error("explicit instantiation of a "
			                    "non-function");
		// Deduce the template arguments from the declared types.
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !resolved; t++)
			resolved = DeduceFunctionTemplateFromTarget(
				*binding->fn_templates[t], declared);
	}
	if (!resolved)
		throw runtime_error("explicit instantiation matches no template");
	FunctionSpecialization& spec =
		*const_cast<FunctionSpecialization*>(resolved);
	if (is_extern)
	{
		// 14.7.2p10: the definition lives in another translation unit;
		// local uses reference the strong external symbol.
		spec.extern_suppressed = true;
		unit_.extern_fn_suppressions.push_back(&spec);
		return;
	}
	// 14.7.2p8: the explicit-instantiation definition demands the body
	// and emits it unconditionally (an emission root); it also lifts a
	// preceding extern declaration's suppression.
	spec.extern_suppressed = false;
	spec.inst_definition = true;
	OnSpecializationOdrUsed(&spec);
	unit_.explicit_fn_instantiations.push_back(&spec);
}

void SemBinder::OnParameterComposed(const string& name,
                                    const TypePtr& type)
{
	if (!param_capture_scope_ || name.empty())
		return;
	// 8.3.5p5: an array- or function-typed parameter declares the
	// adjusted pointer object.
	TypePtr adjusted = type->kind == TK_ARRAY || type->kind == TK_FUNCTION
		? AdjustParameterType(type) : type;
	if (ScopeBinding* existing = FindOwnBinding(*param_capture_scope_,
	                                            name))
	{
		// A pack binding published ahead of the clause (for the
		// trailing-return decltype) carries only the pack facts; its
		// element-0 composition completes the parameter type.
		if (existing->kind == SB_PARAMETER && existing->is_pack &&
		    !existing->type)
			existing->type = adjusted;
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_PARAMETER;
	binding.name = name;
	binding.type = adjusted;
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

