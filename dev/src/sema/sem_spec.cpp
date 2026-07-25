#include "sema/sem_instantiation.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA19 explicit and partial specialization, and variable templates.
// Explicit specializations claim the specialization slot for their
// concrete argument key and bind their own definition through the
// ordinary machinery; partial specializations register argument
// patterns (composed over abstract placeholder slots) that
// specialization lookup matches by structural deduction before the
// primary pattern instantiates. Variable templates resolve per-key to
// objectless constant bindings (every supported use folds).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA19 assignment boundary");
}

// Whether a declarator carries a parameter clause (declares a
// function) at any nesting level.
bool DeclaratorHasParameterClause(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return true;
		if (item.kind == DI_NESTED && item.nested &&
		    DeclaratorHasParameterClause(*item.nested))
			return true;
	}
	return false;
}

}  // namespace

// PA21 alias templates: `template<..> using X = type-id;` captures
// the type-id as the substitution pattern; the name binds as a
// template (SB_CLASS_TEMPLATE with a TMPL_ALIAS record) so template-id
// uses route through the ordinary seam.
void SemBinder::CaptureAliasTemplate(const AstDecl& decl,
                                     const AstDecl& inner)
{
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	if (ScopeBinding* existing = FindOwnBinding(*current_, inner.name))
	{
		if (existing->kind != SB_CLASS_TEMPLATE || !existing->templ ||
		    existing->templ->kind != TMPL_ALIAS)
			throw runtime_error(inner.name +
			                    " redeclared as an alias template");
		// An instantiation re-walk re-captures the same declaration.
		existing->templ->params = params;
		existing->templ->alias_type = inner.type.get();
		return;
	}
	TemplateInfo* tmpl = unit_.templates.Create(TMPL_ALIAS, inner.name,
	                                            current_);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->alias_type = inner.type.get();
	// The anchor stands behind deferred (dependent) alias uses.
	tmpl->anchor = model_.CreateNamedTypeInfo(
		"alias-template " + inner.name, current_, inner.name);
	tmpl->anchor->is_template_anchor = true;
	tmpl->anchor->spec_template = tmpl;
	ScopeBinding binding;
	binding.kind = SB_CLASS_TEMPLATE;
	binding.name = inner.name;
	binding.access = current_access_;
	binding.templ = tmpl;
	AddBinding(*current_, binding);
}

// The substituted type of one alias-template-id use: the arguments
// bind in an alias scope under the template's declaring context
// (14.5.7: names in the type-id resolve there, not at the use site)
// and the type-id resolves through the ordinary builder. Cached per
// argument key.
const ScopeBinding* SemBinder::ResolveAliasTemplateId(
	TemplateInfo& tmpl, const vector<TemplateArg>& args)
{
	// PA34: the builtin __type_pack_element selects its indexed
	// element instead of substituting an aliased type; the builtin
	// __is_nothrow_invocable synthesizes its value record.
	if (&tmpl == type_pack_element_tmpl_.get())
		return ResolveTypePackElementUse(args);
	if (&tmpl == nothrow_invocable_tmpl_.get())
		return ResolveNothrowInvocableUse(args);
	if (!tmpl.alias_type)
		throw runtime_error("alias template " + tmpl.name +
		                    " has no aliased type");
	string key = TemplateArgumentKey(args);
	unique_ptr<ScopeBinding>& slot = tmpl.dependent_uses[key];
	if (slot)
		return slot.get();
	bool dependent_args = false;
	for (size_t i = 0; i < args.size(); i++)
		if (TemplateArgIsDependent(args[i]))
			dependent_args = true;
	Scope* alias_scope = MakeArgumentAliasScope(tmpl, args);
	Scope* saved = current_;
	current_ = alias_scope;
	TypePtr substituted;
	try
	{
		substituted = builder_.ResolveTypeId(*tmpl.alias_type);
	}
	catch (...)
	{
		current_ = saved;
		// A substitution that needs instantiation-time facts
		// (dependent member types) defers inside an abstract pattern:
		// the use re-resolves concretely when its context
		// instantiates.
		if (InAbstractTemplateContext())
		{
			slot.reset(new ScopeBinding());
			slot->kind = SB_TYPE_ALIAS;
			slot->name = tmpl.name;
			slot->type = MakeTemplateSpecType(tmpl.anchor, args);
			slot->owner = tmpl.declaring;
			slot->home = tmpl.declaring;
			return slot.get();
		}
		throw;
	}
	current_ = saved;
	// CWG 1558: when the substitution discarded dependent arguments
	// (void_t's target never mentions its parameters), the use stays
	// deferred so the arguments' validity re-checks at match and
	// instantiation time.
	if (dependent_args && !TypeIsDependent(substituted) &&
	    InAbstractTemplateContext())
	{
		slot.reset(new ScopeBinding());
		slot->kind = SB_TYPE_ALIAS;
		slot->name = tmpl.name;
		slot->type = MakeTemplateSpecType(tmpl.anchor, args);
		slot->owner = tmpl.declaring;
		slot->home = tmpl.declaring;
		return slot.get();
	}
	slot.reset(new ScopeBinding());
	slot->kind = SB_TYPE_ALIAS;
	slot->name = tmpl.name;
	slot->type = substituted;
	slot->owner = tmpl.declaring;
	slot->home = tmpl.declaring;
	return slot.get();
}

// 14.5.4p5: a template-id friend (`operator+<>`) refers to a
// specialization of an already-declared function template; it grants
// access without declaring a new function (and never hides the
// template from resolution).
void SemBinder::BindTemplateIdFriend(Scope* target, const string& name,
                                     const TypePtr& declared)
{
	const ScopeBinding* existing = UnqualifiedLookup(target, name,
	                                                 SLF_ANY);
	if (!existing || existing->kind != SB_FUNCTION ||
	    existing->fn_templates.empty())
		throw runtime_error("template-id friend matches no function "
		                    "template");
	const FunctionSpecialization* spec = 0;
	for (size_t t = 0; !spec && t < existing->fn_templates.size(); t++)
		spec = DeduceFunctionTemplateFromTarget(
			*existing->fn_templates[t], declared);
	if (!spec)
		throw runtime_error("template-id friend matches no function "
		                    "template");
}

void SemBinder::CaptureClassTemplate(const AstDecl& decl,
                                     const AstDecl& inner, bool definition)
{
	const string& name = inner.class_name.parts.back().identifier;
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	// 14.1p11: a class template's parameter pack must be the last
	// parameter (partial specializations are exempt; they register
	// through RegisterClassPartial instead).
	for (size_t i = 0; i + 1 < params.size(); i++)
		if (params[i].pack)
			throw runtime_error("template parameter pack of " + name +
			                    " is not the last parameter");
	TemplateInfo* tmpl = 0;
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		if (existing->kind != SB_CLASS_TEMPLATE)
			throw runtime_error(name + " redeclared as a class template");
		tmpl = existing->templ;
		if (tmpl->params.size() != params.size())
			throw runtime_error("template parameter list of " + name +
			                    " disagrees with its declaration");
		if (definition && tmpl->has_definition)
			throw runtime_error("redefinition of class template " + name);
		// 14.1p10: a redeclaration may add default arguments; the
		// definition's parameter names win (positional identity).
		for (size_t i = 0; i < params.size(); i++)
		{
			if (params[i].kind != tmpl->params[i].kind)
				throw runtime_error("template parameter kind of " +
				                    name + " disagrees with its "
				                    "declaration");
			if ((params[i].default_type &&
			     tmpl->params[i].default_type) ||
			    (params[i].default_expr && tmpl->params[i].default_expr))
				throw runtime_error("template parameter redefines a "
				                    "default argument");
			if (!params[i].default_type)
				params[i].default_type = tmpl->params[i].default_type;
			if (!params[i].default_expr)
				params[i].default_expr = tmpl->params[i].default_expr;
		}
		tmpl->params = params;
	}
	else
	{
		tmpl = unit_.templates.Create(TMPL_CLASS, name, current_);
		tmpl->params = params;
		tmpl->anchor = model_.CreateNamedTypeInfo(
			TypeDisplayName(inner.class_key_spelling, name), current_,
			name);
		tmpl->anchor->is_template_anchor = true;
		tmpl->anchor->spec_template = tmpl;
		tmpl->anchor->class_key = inner.class_key_spelling;
		tmpl->anchor->is_union = inner.class_key == KW_UNION;
		ScopeBinding binding;
		binding.kind = SB_CLASS_TEMPLATE;
		binding.name = name;
		binding.access = current_access_;
		binding.templ = tmpl;
		AddBinding(*current_, binding);
	}
	if (definition)
	{
		tmpl->decl = &decl;
		tmpl->pattern_decl = &inner;
		tmpl->has_definition = true;
		CheckTemplateDefinitionSanity(*tmpl);
		// Specializations named while only the forward declaration was
		// visible stay dormant (14.7.1p1): the first completeness
		// demand instantiates them through the ordinary body path,
		// which also runs the partial-specialization match.
		InstantiateReadyMembers(*tmpl);
	}
	else if (!tmpl->decl)
	{
		tmpl->decl = &decl;
		tmpl->pattern_decl = &inner;
	}
}

// A merged redeclaration's definition adoption: the declaration's
// defaults accumulate onto the definition head (14.1p10), renamed
// template parameters re-alias in existing specializations' argument
// scopes, and pending bodies instantiate against the new pattern.
void SemBinder::AdoptFunctionTemplateDefinition(
	TemplateInfo& merged, const AstDecl& decl, const AstDecl& inner,
	vector<TemplateParam>& params, const string& name,
	bool replace_instantiated)
{
	// 14.7.3p18: an explicit member definition replaces only a
	// definition the enclosing specialization instantiated from the
	// primary's pattern; a duplicate explicit definition is an ODR
	// redefinition either way.
	if (merged.has_definition &&
	    !(replace_instantiated && merged.definition_instantiated))
		throw runtime_error("redefinition of function template " + name);
	merged.decl = &decl;
	merged.pattern_decl = &inner;
	merged.has_definition = true;
	merged.definition_instantiated = instantiating_;
	// 14.1p10: default template arguments accumulate across
	// declarations; the definition keeps the declaration's.
	for (size_t i = 0; i < params.size() && i < merged.params.size(); i++)
	{
		if (!params[i].default_type && merged.params[i].default_type)
			params[i].default_type = merged.params[i].default_type;
		if (!params[i].default_expr && merged.params[i].default_expr)
			params[i].default_expr = merged.params[i].default_expr;
	}
	// The definition may rename template parameters; existing
	// specializations bound the declaration's names, so each renamed
	// parameter re-aliases in their argument scopes before any pending
	// body composes against the new names.
	for (map<string, unique_ptr<FunctionSpecialization>>::iterator it =
	         merged.fn_specs.begin();
	     it != merged.fn_specs.end(); ++it)
	{
		FunctionSpecialization* spec = it->second.get();
		if (!spec || !spec->param_scope)
			continue;
		for (size_t i = 0; i < params.size() && i < merged.params.size();
		     i++)
		{
			const string& fresh = params[i].name;
			const string& old = merged.params[i].name;
			if (fresh.empty() || fresh == old)
				continue;
			map<string, size_t>::const_iterator at =
				spec->param_scope->binding_index.find(old);
			if (at == spec->param_scope->binding_index.end() ||
			    spec->param_scope->binding_index.count(fresh))
				continue;
			ScopeBinding alias = spec->param_scope->bindings[at->second];
			alias.name = fresh;
			AddBinding(*spec->param_scope, alias);
		}
	}
	merged.params = params;
	merged.pattern_ready = false;
	InstantiatePendingFunctions(merged);
}

TemplateInfo* SemBinder::CaptureFunctionTemplate(const AstDecl& decl,
                                                 const AstDecl& inner,
                                                 bool as_friend,
                                                 Scope* friend_home,
                                                 bool replace_instantiated)
{
	const AstName* id = inner.kind == DK_FUNCTION
		? inner.declarator->IdName()
		: inner.declarators[0].declarator->IdName();
	const string name = DeclaredFunctionName(id->parts.back());
	bool definition = inner.kind == DK_FUNCTION;
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);

	ScopeBinding* binding = FindOwnBinding(*current_, name);
	if (binding && binding->kind != SB_FUNCTION)
		throw runtime_error(name + " redeclared as a function template");
	// 14.5.4: each enclosing instantiation declares its own friend;
	// captures only merge with records from the same home (the
	// instantiation's alias scope, or the namespace for plain-class
	// friends).
	Scope* home = as_friend && friend_home ? friend_home : current_;
	TemplateInfo* merged = 0;
	if (binding)
	{
		// A declaration and its definition (possibly with renamed
		// parameters) merge positionally; other same-name templates
		// overload.
		for (size_t i = 0; i < binding->fn_templates.size(); i++)
		{
			TemplateInfo* other = binding->fn_templates[i];
			if (other->params.size() != params.size())
				continue;
			if (as_friend && other->declaring != home)
				continue;
			if (SameFunctionTemplateSignature(*other, decl, inner))
			{
				merged = other;
				break;
			}
		}
		// An out-of-class definition may spell a template-head type
		// through the class's own typedefs; pair it with the unique
		// definition-less declaration that matches everywhere else (a
		// pattern-instantiated definition is replaceable, 14.7.3p18).
		if (!merged && definition)
			for (size_t i = 0; i < binding->fn_templates.size(); i++)
			{
				TemplateInfo* other = binding->fn_templates[i];
				if (other->params.size() != params.size() ||
				    (other->has_definition &&
				     !(replace_instantiated &&
				       other->definition_instantiated)))
					continue;
				if (as_friend && other->declaring != home)
					continue;
				if (SameFunctionTemplateSignature(*other, decl, inner,
				                                  false))
				{
					merged = other;
					break;
				}
			}
	}
	if (merged)
	{
		if (definition && merged->pattern_decl != &inner)
			AdoptFunctionTemplateDefinition(*merged, decl, inner, params,
			                                name, replace_instantiated);
		// A real namespace-scope redeclaration makes a hidden friend
		// template visible (7.3.1.2p3).
		if (!as_friend && binding && !binding->fn_adl_only.empty() &&
		    !binding->type)
			binding->fn_adl_only.assign(binding->fn_adl_only.size(),
			                            false);
		return merged;
	}
	TemplateInfo* tmpl = unit_.templates.Create(TMPL_FUNCTION, name,
	                                            current_);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition = definition;
	tmpl->definition_instantiated = definition && instantiating_;
	// Slot names follow the first declaration (unnamed definition
	// parameters fall back to them).
	if (const AstDeclarator* first_declarator =
	        inner.kind == DK_FUNCTION
	            ? inner.declarator.get()
	            : inner.declarators[0].declarator.get())
		CollectDeclaredParamNames(*first_declarator,
		                          tmpl->declared_param_names);
	// 14.3p1 shape checks apply to declarations too (a pack-expansion
	// argument targeting a non-pack parameter is ill-formed without
	// any instantiation).
	ValidateSignatureTemplateIds(inner.specifiers,
	                             inner.kind == DK_FUNCTION
	                                 ? inner.declarator.get()
	                                 : inner.declarators[0].declarator.get());
	if (definition)
		CheckTemplateDefinitionSanity(*tmpl);
	if (!binding)
	{
		ScopeBinding fresh;
		fresh.kind = SB_FUNCTION;
		fresh.name = name;
		fresh.access = current_access_;
		binding = &AddBinding(*current_, fresh);
		// 7.3.1.2p3: a template declared only by friend declarations
		// stays invisible to ordinary lookup (ADL still finds it).
		if (as_friend)
			binding->fn_adl_only.assign(1, true);
	}
	else if (!as_friend && !binding->fn_adl_only.empty() && !binding->type)
		binding->fn_adl_only.assign(binding->fn_adl_only.size(), false);
	binding->fn_templates.push_back(tmpl);
	return tmpl;
}

// --- shared pattern composition and matching --------------------------------

// The abstract argument pattern of one partial specialization: the
// template-id's arguments resolved with the partial's own parameters
// bound to placeholder slots.
vector<TemplateArg> SemBinder::ComposePartialPattern(
	TemplateInfo& primary, const vector<TemplateParam>& params,
	const AstNamePart& part)
{
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                  primary.declaring);
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
		else if (params[i].kind == TPK_TEMPLATE)
		{
			// PA21: the parameter binds a placeholder template whose
			// anchor carries the slot for deduction.
			binding.kind = SB_CLASS_TEMPLATE;
			binding.templ = TemplateParamPlaceholder(params[i], i);
		}
		else
		{
			binding.kind = SB_VARIABLE;
			binding.no_object = true;
		}
		binding.param_index = (int)i;
		binding.is_pack = params[i].pack;
		AddBinding(*scope, binding);
	}
	Scope* saved = current_;
	current_ = scope;
	vector<TemplateArg> pattern;
	try
	{
		pattern = ResolveTemplateArgumentList(primary, part);
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	return pattern;
}

// The partial specialization whose pattern deduces from the concrete
// arguments; among several matches the most specialized wins
// (14.5.4.1 via the 14.5.5.2 ordering subset), and a tie is an
// ambiguity error. Returns -1 when none matches.
int SemBinder::MatchPartialSpecialization(TemplateInfo& tmpl,
                                          const vector<TemplateArg>& args,
                                          vector<TemplateArg>& bound)
{
	vector<size_t> matches;
	// The defaulted-tail allowance (a pattern shorter than the
	// argument list) only applies when the primary's length is fixed;
	// with a primary parameter pack the pattern must consume the whole
	// list.
	bool allow_trailing =
		TemplatePackIndex(tmpl.params) == tmpl.params.size();
	for (size_t p = 0; p < tmpl.partials.size(); p++)
	{
		const PartialSpecialization& partial = tmpl.partials[p];
		vector<TemplateArg> candidate(partial.params.size());
		for (size_t i = 0; i < partial.params.size(); i++)
			candidate[i].is_pack_slot = partial.params[i].pack;
		if (!DeduceTemplateArgs(partial.pattern, args, candidate,
		                        allow_trailing))
			continue;
		bool complete = true;
		for (size_t i = 0; i < candidate.size(); i++)
		{
			if (candidate[i].is_pack_slot)
				complete = complete && candidate[i].pack_done;
			else if (!candidate[i].is_value && !candidate[i].type &&
			         !candidate[i].template_entity)
				complete = false;
		}
		if (!complete)
			continue;
		// PA21: deferred type slots re-resolve under the deduced
		// bindings and must match the concrete argument (a failed
		// substitution disqualifies the candidate).
		if (!CheckDependentPatternSlots(tmpl, partial, args, candidate))
			continue;
		matches.push_back(p);
	}
	if (matches.empty())
		return -1;
	size_t best = matches[0];
	for (size_t m = 1; m < matches.size(); m++)
	{
		const PartialSpecialization& challenger = tmpl.partials[matches[m]];
		const PartialSpecialization& champion = tmpl.partials[best];
		bool challenger_wins =
			PartialAtLeastAsSpecialized(challenger, champion) &&
			!PartialAtLeastAsSpecialized(champion, challenger);
		if (challenger_wins)
			best = matches[m];
	}
	// The winner must beat every other match, not just its neighbors.
	for (size_t m = 0; m < matches.size(); m++)
	{
		if (matches[m] == best)
			continue;
		if (!PartialAtLeastAsSpecialized(tmpl.partials[best],
		                                 tmpl.partials[matches[m]]) ||
		    PartialAtLeastAsSpecialized(tmpl.partials[matches[m]],
		                                tmpl.partials[best]))
			throw runtime_error("ambiguous partial specializations of " +
			                    tmpl.name);
	}
	bound.assign(tmpl.partials[best].params.size(), TemplateArg());
	for (size_t i = 0; i < bound.size(); i++)
		bound[i].is_pack_slot = tmpl.partials[best].params[i].pack;
	DeduceTemplateArgs(tmpl.partials[best].pattern, args, bound,
	                   allow_trailing);
	return (int)best;
}

// The deferred (dependent_type) slots of a matched pattern:
// re-resolve each under the deduced parameter bindings and compare
// with the concrete argument. False when a substitution fails or
// disagrees (the candidate does not match, 14.8.2 in the PA21 slice).
bool SemBinder::CheckDependentPatternSlots(
	TemplateInfo& tmpl, const PartialSpecialization& partial,
	const vector<TemplateArg>& args, const vector<TemplateArg>& bound)
{
	bool any = false;
	for (size_t i = 0; i < partial.pattern.size(); i++)
		if (partial.pattern[i].dependent_type ||
		    (!partial.pattern[i].is_value &&
		     TypeIsDependentAliasUse(partial.pattern[i].type)))
			any = true;
	if (!any)
		return true;
	TemplateInfo shadow;
	shadow.params = partial.params;
	shadow.declaring = tmpl.declaring;
	shadow.capture_seq = tmpl.capture_seq;
	Scope* alias_scope = MakeArgumentAliasScope(shadow, bound);
	size_t ai = 0;
	for (size_t i = 0; i < partial.pattern.size(); i++)
	{
		const TemplateArg& slot = partial.pattern[i];
		if (slot.pack_pattern)
		{
			// A pack slot absorbs the remaining run less the fixed
			// tail; deferred slots after a top-level pack stay
			// unsupported (none in the suites).
			size_t fixed_tail = partial.pattern.size() - i - 1;
			ai = args.size() >= fixed_tail ? args.size() - fixed_tail
			                               : args.size();
			continue;
		}
		if (ai >= args.size())
			return false;
		if (slot.dependent_type)
		{
			Scope* saved = current_;
			current_ = alias_scope;
			TypePtr resolved;
			try
			{
				resolved = builder_.ResolveTypeId(*slot.dependent_type);
			}
			catch (const InstantiationBodyFault&)
			{
				current_ = saved;
				throw;
			}
			catch (const std::exception&)
			{
				current_ = saved;
				return false;
			}
			current_ = saved;
			const TemplateArg& concrete = args[ai];
			if (concrete.is_value || !concrete.type ||
			    !TypeEquals(resolved, concrete.type))
				return false;
		}
		else if (!slot.is_value && TypeIsDependentAliasUse(slot.type))
		{
			// 14.5.7p2: the alias use re-substitutes with the deduced
			// bindings; a substitution failure means no match.
			TypePtr resolved;
			try
			{
				resolved = ResolveDependentAliasUse(slot.type,
				                                    alias_scope, bound);
			}
			catch (const InstantiationBodyFault&)
			{
				throw;
			}
			catch (const std::exception&)
			{
				return false;
			}
			const TemplateArg& concrete = args[ai];
			if (!resolved || concrete.is_value || !concrete.type ||
			    !TypeEquals(resolved, concrete.type))
				return false;
		}
		ai++;
	}
	return true;
}

// One pattern argument slot substituted under the deduced bindings:
// parameter references take their bound slots, pack patterns splice
// their runs, deferred type-ids re-resolve under `alias_scope`, and
// nested alias/template-template applications resolve recursively
// (a substitution failure propagates so the match check can reject).
void SemBinder::SubstituteSlotArg(const TemplateArg& slot,
                                  Scope* alias_scope,
                                  const vector<TemplateArg>& bound,
                                  vector<TemplateArg>& out)
{
	// A pack pattern over a bound pack slot splices its run.
	if (slot.pack_pattern && !slot.is_value && slot.type &&
	    slot.type->kind == TK_TYPE_PARAM &&
	    slot.type->named->param_index >= 0 &&
	    (size_t)slot.type->named->param_index < bound.size() &&
	    bound[slot.type->named->param_index].pack_done)
	{
		const vector<TemplateArg>& run =
			bound[slot.type->named->param_index].pack_elements;
		for (size_t k = 0; k < run.size(); k++)
			out.push_back(run[k]);
		return;
	}
	TemplateArg arg = slot;
	if (slot.dependent_type)
	{
		Scope* saved = current_;
		current_ = alias_scope;
		try
		{
			arg = TemplateArg(
				builder_.ResolveTypeId(*slot.dependent_type));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	else if (!slot.is_value && slot.type &&
	         slot.type->kind == TK_TYPE_PARAM &&
	         slot.type->named->param_index >= 0 &&
	         (size_t)slot.type->named->param_index < bound.size())
		arg = bound[slot.type->named->param_index];
	else if (!slot.is_value && TypeIsDependentAliasUse(slot.type))
	{
		TypePtr resolved = ResolveDependentAliasUse(slot.type,
		                                            alias_scope, bound);
		if (resolved)
			arg = TemplateArg(resolved);
	}
	else if (!slot.is_value && slot.type &&
	         slot.type->kind == TK_TEMPLATE_SPEC &&
	         slot.type->named->is_template_anchor &&
	         slot.type->named->param_index >= 0 &&
	         (size_t)slot.type->named->param_index < bound.size() &&
	         bound[slot.type->named->param_index].template_entity)
	{
		// A bound template-template application (`Op<Args...>`).
		TemplateInfo& applied = *const_cast<TemplateInfo*>(
			bound[slot.type->named->param_index].template_entity);
		vector<TemplateArg> inner;
		for (size_t i = 0; i < slot.type->targs.size(); i++)
			SubstituteSlotArg(slot.type->targs[i], alias_scope, bound,
			                  inner);
		if (applied.kind == TMPL_ALIAS)
		{
			const ScopeBinding* resolved =
				ResolveAliasTemplateId(applied, inner);
			if (resolved && resolved->type)
				arg = TemplateArg(resolved->type);
		}
		else if (ClassSpecialization* spec =
		             EnsureClassSpecialization(applied, inner))
			arg = TemplateArg(spec->self.type);
	}
	else if (slot.is_value && slot.value_param >= 0 &&
	         (size_t)slot.value_param < bound.size())
		arg = bound[slot.value_param];
	else if (slot.is_value && slot.dependent_value)
	{
		// A dependent value expression (`!!Property::template v<T>`)
		// re-evaluates under the deduced bindings; an evaluation
		// failure propagates so the match check can reject.
		Scope* saved = current_;
		current_ = alias_scope;
		try
		{
			ConstValue value;
			try
			{
				value = EvaluateConstExpr(*slot.dependent_value, *this);
			}
			catch (const std::exception&)
			{
				if (!TryFullConstant(*slot.dependent_value, value))
					throw;
			}
			arg.value_type = value.type;
			arg.value_bits = value.bits;
			arg.value_param = -1;
			arg.dependent_value = 0;
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	out.push_back(arg);
}

// A dependent alias-template use kept as a TEMPLATE_SPEC pattern:
// re-resolves under `alias_scope` with the deduced `bound` slots
// substituted into its argument patterns. Null when a nested pattern
// stays dependent.
TypePtr SemBinder::ResolveDependentAliasUse(const TypePtr& pattern,
                                            Scope* alias_scope,
                                            const vector<TemplateArg>& bound)
{
	if (!TypeIsDependentAliasUse(pattern))
		return TypePtr();
	TemplateInfo& alias =
		*const_cast<TemplateInfo*>(pattern->named->spec_template);
	vector<TemplateArg> concrete;
	for (size_t i = 0; i < pattern->targs.size(); i++)
		SubstituteSlotArg(pattern->targs[i], alias_scope, bound,
		                  concrete);
	for (size_t i = 0; i < concrete.size(); i++)
		if (TemplateArgIsDependent(concrete[i]))
			return TypePtr();
	const ScopeBinding* resolved = ResolveAliasTemplateId(alias, concrete);
	return resolved ? resolved->type : TypePtr();
}

// --- explicit specialization entry -------------------------------------------

// 14.7.3p18: `template<> template<...> R Owner<args>::member` declares
// (or defines) a member template of one concrete specialization; the
// inner template captures against the owner's member scope.
void SemBinder::BindMemberTemplateOfSpecialization(const AstDecl& inner)
{
	const AstDecl* leaf = inner.inner.get();
	const AstName* id = 0;
	if (leaf)
	{
		if (leaf->kind == DK_CLASS || leaf->kind == DK_CLASS_FORWARD)
			id = &leaf->class_name;
		else if (leaf->kind == DK_FUNCTION ||
		         leaf->kind == DK_SPECIAL_MEMBER_DEFINITION ||
		         leaf->kind == DK_SPECIAL_MEMBER_DECLARATION)
			id = leaf->declarator ? leaf->declarator->IdName() : 0;
		else if (leaf->kind == DK_SIMPLE &&
		         leaf->declarators.size() == 1 &&
		         leaf->declarators[0].declarator)
			id = leaf->declarators[0].declarator->IdName();
	}
	if (!id || id->parts.size() < 2)
		throw OutsideBoundary("explicit specialization form");
	Scope* declaring = ResolvePrefixScope(*id);
	if (!declaring || declaring->kind != SCOPE_CLASS)
		throw OutsideBoundary("explicit specialization form");
	// 14.7.3p18: this definition replaces the member definition the
	// enclosing specialization instantiated from the primary's pattern
	// (the capture rejects a duplicate explicit definition).
	CaptureQualifiedMemberTemplate(inner, *leaf, declaring, true);
}

void SemBinder::BindExplicitSpecialization(const AstDecl& decl)
{
	if (!decl.inner)
		throw OutsideBoundary("explicit specialization form");
	const AstDecl& inner = *decl.inner;
	if (current_->kind != SCOPE_NAMESPACE)
		throw OutsideBoundary("block-scope explicit specialization");
	switch (inner.kind)
	{
	case DK_CLASS:
	case DK_CLASS_FORWARD:
	{
		if (!inner.has_name || inner.class_name.parts.empty() ||
		    inner.class_name.parts.back().kind != NP_TEMPLATE_ID)
			throw OutsideBoundary("explicit specialization name form");
		// PA21: a qualified name explicitly specializes a member class
		// template inside its owner's scope (or a namespace-qualified
		// class template in its home namespace).
		if (inner.class_name.parts.size() > 1)
		{
			Scope* declaring = ResolvePrefixScope(inner.class_name);
			if (!declaring || (declaring->kind != SCOPE_CLASS &&
			                   declaring->kind != SCOPE_NAMESPACE))
				throw OutsideBoundary("explicit specialization name "
				                      "form");
			Scope* saved = current_;
			current_ = declaring;
			try
			{
				BindClassExplicitSpecialization(inner);
			}
			catch (...)
			{
				current_ = saved;
				throw;
			}
			current_ = saved;
			return;
		}
		BindClassExplicitSpecialization(inner);
		return;
	}
	case DK_FUNCTION:
	{
		const AstName* id =
			inner.declarator ? inner.declarator->IdName() : 0;
		if (id && id->parts.size() > 1)
		{
			BindMemberExplicitSpecialization(inner, *id);
			return;
		}
		BindFunctionExplicitSpecialization(inner);
		return;
	}
	case DK_SIMPLE:
	{
		const AstDeclarator* d = inner.declarators.size() == 1
			? inner.declarators[0].declarator.get() : 0;
		const AstName* id = d ? d->IdName() : 0;
		if (id && id->parts.size() > 1)
		{
			BindMemberExplicitSpecialization(inner, *id);
			return;
		}
		// A declarator-id directly followed by a parameter clause is a
		// function specialization declared without a body
		// (`template<> R f(args);`, 14.7.3p11 deduced form included).
		if (d)
			for (size_t i = 0; i + 1 < d->items.size(); i++)
				if (d->items[i].kind == DI_ID &&
				    d->items[i + 1].kind == DI_PARAMS)
				{
					BindFunctionExplicitSpecialization(inner, d);
					return;
				}
		BindVariableExplicitSpecialization(inner);
		return;
	}
	case DK_SPECIAL_MEMBER_DEFINITION:
	case DK_SPECIAL_MEMBER_DECLARATION:
	{
		const AstName* id =
			inner.declarator ? inner.declarator->IdName() : 0;
		if (id && id->parts.size() > 1)
		{
			BindMemberExplicitSpecialization(inner, *id);
			return;
		}
		throw OutsideBoundary("explicit specialization form");
	}
	case DK_TEMPLATE:
		BindMemberTemplateOfSpecialization(inner);
		return;
	default:
		throw runtime_error("explicit specialization form kind " +
		                    std::to_string((int)inner.kind) +
		                    " is outside the PA19 boundary");
	}
}

// PA21 14.7.3: an explicit specialization of one member of a class
// -template specialization (`template<> int tag<int>::id() {..}`,
// `template<> const int code<int>::value = 7;`, qualified
// constructors). Resolving the qualifier instantiates (or refreshes)
// the enclosing specialization; the declaration itself then binds
// through the ordinary qualified-member machinery as a source-owned
// strong definition, and the primary's registered member definitions
// stop instantiating for that name.
void SemBinder::BindMemberExplicitSpecialization(const AstDecl& inner,
                                                 const AstName& id)
{
	Scope* declaring = ResolvePrefixScope(id);
	if (!declaring || declaring->kind != SCOPE_CLASS)
		throw OutsideBoundary("explicit specialization declarator");
	const NamedTypeInfo* entity = model_.ScopeEntity(declaring);
	if (ClassSpecialization* record = FindSpecializationRecord(entity))
	{
		// 14.7.3p5: members of an explicit specialization are ordinary
		// members; a template<> header on their definitions is
		// ill-formed.
		if (record->explicit_spec)
			throw runtime_error("template<> header on a member of an "
			                    "explicit specialization");
		const AstNamePart& terminal = id.parts.back();
		string name = terminal.kind == NP_IDENTIFIER
			? terminal.identifier : DeclaredFunctionName(terminal);
		record->member_spec_names[name] = true;
		// A pattern-instantiated constructor definition yields to the
		// explicit one.
		if (inner.kind == DK_SPECIAL_MEMBER_DEFINITION &&
		    entity && !terminal.tilde)
			if (ClassInfo* cls = unit_.classes.Find(entity))
				for (size_t i = 0; i < cls->ctors.size(); i++)
					cls->ctors[i].definition = 0;
	}
	// A bare specialization declaration only reserves the member (the
	// declared signature already exists on the instantiated class).
	if (inner.kind == DK_SPECIAL_MEMBER_DECLARATION &&
	    !inner.special_init)
		return;
	if (inner.kind == DK_SIMPLE)
	{
		bool any_init = false;
		for (size_t i = 0; i < inner.declarators.size(); i++)
			if (inner.declarators[i].init)
				any_init = true;
		// 14.7.3p15: without an initializer the explicit
		// specialization is a declaration, not a definition - a
		// static data member's symbol stays external (the hosted
		// __timepunct_cache<char>::_S_timezones shape) and the
		// generic pattern stops instantiating for the name.
		if (!any_init)
			return;
	}
	BindDeclaration(inner);
}

// --- class explicit specialization -------------------------------------------

void SemBinder::BindClassExplicitSpecialization(const AstDecl& inner)
{
	const AstNamePart& part = inner.class_name.parts.back();
	const ScopeBinding* found =
		UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found || found->kind != SB_CLASS_TEMPLATE || !found->templ)
		throw runtime_error(part.identifier +
		                    " does not name a class template");
	TemplateInfo& tmpl = *found->templ;
	size_t collapse_before = collapsed_alias_uses_;
	vector<TemplateArg> args = ResolveTemplateArgumentList(tmpl, part);
	bool collapse_now = collapsed_alias_uses_ != collapse_before;
	string key = TemplateArgumentKey(args);
	unique_ptr<ClassSpecialization>& slot = tmpl.class_specs[key];
	if (!slot)
	{
		slot.reset(new ClassSpecialization());
		ClassSpecialization* fresh = slot.get();
		fresh->owner = &tmpl;
		fresh->args = args;
		fresh->key = key;
		const string spec_name =
			tmpl.name + TemplateArgumentSpelling(args);
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			tmpl.anchor->class_key + " " +
				QualifiedScopePath(tmpl.declaring) + spec_name,
			tmpl.declaring, spec_name);
		info->is_union = tmpl.anchor->is_union;
		info->class_key = tmpl.anchor->class_key;
		info->spec_template = &tmpl;
		info->spec_args = args;
		fresh->entity = info;
		fresh->self.kind = SB_TYPE;
		fresh->self.name = tmpl.name;
		fresh->self.type = MakeNamedType(TK_CLASS, info);
		fresh->self.owner = tmpl.declaring;
		fresh->self.home = tmpl.declaring;
	}
	ClassSpecialization& spec = *slot.get();
	bool was_explicit = spec.explicit_spec;
	bool collapse_involved = collapse_now || spec.alias_collapsed;
	spec.explicit_spec = true;
	if (collapse_now)
		spec.alias_collapsed = true;
	if (inner.kind == DK_CLASS_FORWARD)
		return;  // a declaration reserves the key
	if (spec.instantiated && (was_explicit || spec.hard_used))
	{
		// PA34 hosted alias collapse: the _FloatN spellings resolve
		// to the standard floating types, so glibc's per-format
		// specialization sets (the iseqsig helpers) can land twice on
		// one key. The first explicit definition wins - only when a
		// collapsed alias spelling was involved on either landing; a
		// plain duplicate stays a redefinition error, and a use before
		// the specialization stays ill-formed.
		if (was_explicit && collapse_involved)
			return;
		throw runtime_error("explicit specialization of " + tmpl.name +
		                    " after its instantiation");
	}
	if (spec.instantiated)
	{
		// 14.7.3p6 makes a use before the explicit specialization
		// ill-formed (no diagnostic required); the checked behavior
		// re-binds the entity from the explicit definition and
		// discards the stale primary instantiation.
		NamedTypeInfo* stale = model_.MutableInfo(spec.entity);
		stale->complete = false;
		stale->is_defined = false;
		if (ClassInfo* record = unit_.classes.Find(spec.entity))
		{
			*record = ClassInfo();
			record->entity = spec.entity;
		}
		spec.from_partial = false;
		spec.statics_demanded = false;
		spec.members_done.clear();
	}
	spec.instantiated = true;
	spec.param_scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                      tmpl.declaring);
	ScopeBinding injected;
	injected.kind = SB_TYPE;
	injected.name = tmpl.name;
	injected.type = spec.self.type;
	AddBinding(*spec.param_scope, injected);
	// The members of an explicit specialization are ordinary members
	// (14.7.3): no weak-instantiation marking.
	InstantiationContext context(*this, spec.param_scope, false);
	bool saved_allow = allow_qualified_class_name_;
	allow_qualified_class_name_ = true;
	try
	{
		BindClass(inner, true);
	}
	catch (...)
	{
		allow_qualified_class_name_ = saved_allow;
		throw;
	}
	allow_qualified_class_name_ = saved_allow;
}

// --- function explicit specialization ----------------------------------------

void SemBinder::BindFunctionExplicitSpecialization(
	const AstDecl& inner, const AstDeclarator* declarator)
{
	if (!declarator)
		declarator = inner.declarator.get();
	const AstName* id = declarator ? declarator->IdName() : 0;
	if (!id || id->parts.size() != 1)
		throw OutsideBoundary("explicit specialization declarator");
	const AstNamePart& terminal = id->parts.back();
	string name = terminal.kind == NP_TEMPLATE_ID
		? terminal.identifier : DeclaredFunctionName(terminal);
	const ScopeBinding* binding =
		UnqualifiedLookup(current_, name, SLF_ANY);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error(name + " does not name a function template");
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(inner.specifiers, true);
	FunctionSpecialization* spec = 0;
	if (terminal.kind == NP_TEMPLATE_ID)
	{
		// 14.7.3 with 14.8.2.6: among same-name templates accepting
		// the explicit arguments, the declared parameter list selects
		// the specialized one.
		TypePtr declared;
		try
		{
			DeclaratorInfo composed = builder_.ComposeDeclarator(
				declarator, specs.type);
			if (composed.type->kind == TK_FUNCTION)
				declared = composed.type;
		}
		catch (const std::exception&)
		{
			declared = TypePtr();
		}
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !spec; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
				continue;
			vector<TemplateArg> args;
			FunctionSpecialization* candidate;
			try
			{
				args = ResolveTemplateArgumentList(tmpl, terminal);
				candidate = EnsureFunctionSpecialization(tmpl, args);
			}
			catch (const std::exception&)
			{
				continue;
			}
			if (declared && !TypeEquals(candidate->type, declared))
				continue;
			spec = candidate;
		}
	}
	else
	{
		// `template<> int digits(unsigned int)`: the specialized
		// arguments deduce from the declared signature (14.7.3p11).
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			declarator, specs.type);
		if (composed.type->kind != TK_FUNCTION)
			throw runtime_error("explicit specialization of a "
			                    "non-function");
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !spec; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			const FunctionSpecialization* deduced =
				DeduceFunctionTemplateFromTarget(tmpl, composed.type);
			if (deduced)
				spec = const_cast<FunctionSpecialization*>(deduced);
		}
	}
	if (!spec)
		throw runtime_error("explicit specialization matches no "
		                    "template");
	if (!inner.body)
	{
		// 14.7.3p6: a declaration of the specialization without a
		// definition (basic_string.h declares `template<>
		// basic_istream<char>& operator>>(...)`, defined in the
		// library): the primary pattern no longer instantiates for
		// these arguments and uses reference the external strong
		// symbol, like an extern explicit-instantiation declaration.
		spec->extern_suppressed = true;
		unit_.extern_fn_suppressions.push_back(spec);
		return;
	}
	if (spec->body_emitted)
		throw runtime_error("redefinition of specialization " +
		                    spec->name);
	spec->extern_suppressed = false;
	spec->explicit_def = &inner;
	spec->explicit_inline = specs.is_inline;
	// A definition emits at its point of declaration (strong unless
	// inline); uses before it deduced only the signature.
	if (inner.body)
		InstantiateFunctionBody(*spec->owner, *spec);
}

// --- class partial specialization ---------------------------------------------

void SemBinder::RegisterClassPartial(const AstDecl& decl,
                                     const AstDecl& inner)
{
	const AstNamePart& part = inner.class_name.parts.back();
	const ScopeBinding* found =
		UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found || found->kind != SB_CLASS_TEMPLATE || !found->templ)
		throw runtime_error(part.identifier +
		                    " does not name a class template");
	TemplateInfo& tmpl = *found->templ;
	PartialSpecialization partial;
	CollectTemplateParams(decl, partial.params);
	partial.pattern = ComposePartialPattern(tmpl, partial.params, part);
	partial.decl = inner.kind == DK_CLASS ? &inner : 0;
	// A redeclaration merges onto the recorded pattern (a forward
	// declaration gains its definition later).
	partial.pattern_key = TemplateArgumentKey(partial.pattern);
	for (size_t p = 0; p < tmpl.partials.size(); p++)
	{
		PartialSpecialization& existing = tmpl.partials[p];
		if (existing.params.size() != partial.params.size() ||
		    existing.pattern_key != partial.pattern_key)
			continue;
		if (partial.decl)
		{
			if (existing.decl)
				throw runtime_error("redefinition of a partial "
				                    "specialization of " + tmpl.name);
			existing.params = partial.params;
			existing.pattern = partial.pattern;
			existing.decl = partial.decl;
		}
		return;
	}
	tmpl.partials.push_back(partial);
}

void SemBinder::InstantiateClassFromPartial(
	TemplateInfo& tmpl, ClassSpecialization& spec, int partial_index,
	const vector<TemplateArg>& bound)
{
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	const PartialSpecialization& partial = tmpl.partials[partial_index];
	spec.instantiated = true;
	spec.lazily_instantiated = instantiating_;
	spec.from_partial = true;
	spec.partial_index = partial_index;
	spec.partial_bound = bound;
	TemplateInfo shadow;
	shadow.params = partial.params;
	shadow.declaring = tmpl.declaring;
	shadow.capture_seq = tmpl.capture_seq;
	spec.param_scope = MakeArgumentAliasScope(shadow, bound);
	ScopeBinding injected;
	injected.kind = SB_TYPE;
	injected.name = tmpl.name;
	injected.type = spec.self.type;
	AddBinding(*spec.param_scope, injected);
	InstantiationContext context(*this, spec.param_scope, true);
	bool saved_allow = allow_qualified_class_name_;
	allow_qualified_class_name_ = true;
	try
	{
		BindClass(*partial.decl, true);
	}
	catch (...)
	{
		allow_qualified_class_name_ = saved_allow;
		// A failed body must not leave a hollow "instantiated" record
		// (mirrors InstantiateClassSpecialization): reset the entity
		// so the next completeness demand re-instantiates - the
		// end-of-unit body retries heal failures caused by a class
		// still open in the demanding context. Repeat failures report
		// softly (InstantiateSpecializationBody), so probes stay
		// tolerant of candidates that legitimately never instantiate.
		spec.instantiated = false;
		NamedTypeInfo* stale = model_.MutableInfo(spec.entity);
		stale->complete = false;
		stale->is_defined = false;
		if (ClassInfo* record = unit_.classes.Find(spec.entity))
		{
			*record = ClassInfo();
			record->entity = spec.entity;
		}
		throw;
	}
	allow_qualified_class_name_ = saved_allow;
	if (Scope* members = model_.MemberScope(spec.entity))
		members->name = spec.entity->name;
}
