#include "sema/sem_binder.h"

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
	for (size_t p = 0; p < tmpl.partials.size(); p++)
	{
		const PartialSpecialization& partial = tmpl.partials[p];
		vector<TemplateArg> candidate(partial.params.size());
		for (size_t i = 0; i < partial.params.size(); i++)
			candidate[i].is_pack_slot = partial.params[i].pack;
		if (!DeduceTemplateArgs(partial.pattern, args, candidate))
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
		if (complete)
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
	DeduceTemplateArgs(tmpl.partials[best].pattern, args, bound);
	return (int)best;
}

// --- explicit specialization entry -------------------------------------------

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
		if (!inner.has_name || inner.class_name.parts.size() != 1 ||
		    inner.class_name.parts.back().kind != NP_TEMPLATE_ID)
			throw OutsideBoundary("explicit specialization name form");
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
		const AstName* id = inner.declarators.size() == 1 &&
			inner.declarators[0].declarator
			? inner.declarators[0].declarator->IdName() : 0;
		if (id && id->parts.size() > 1)
		{
			BindMemberExplicitSpecialization(inner, *id);
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
	default:
		throw OutsideBoundary("explicit specialization form");
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
		bool declares_function = inner.declarators.size() == 1 &&
			inner.declarators[0].declarator &&
			DeclaratorHasParameterClause(
				*inner.declarators[0].declarator);
		if (!any_init && declares_function)
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
	vector<TemplateArg> args = ResolveTemplateArgumentList(tmpl, part);
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
	spec.explicit_spec = true;
	if (inner.kind == DK_CLASS_FORWARD)
		return;  // a declaration reserves the key
	if (spec.instantiated && was_explicit)
		throw runtime_error("explicit specialization of " + tmpl.name +
		                    " after its instantiation");
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

void SemBinder::BindFunctionExplicitSpecialization(const AstDecl& inner)
{
	const AstName* id = inner.declarator ? inner.declarator->IdName() : 0;
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
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !spec; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
				continue;
			vector<TemplateArg> args;
			try
			{
				args = ResolveTemplateArgumentList(tmpl, terminal);
			}
			catch (const std::exception&)
			{
				continue;
			}
			spec = EnsureFunctionSpecialization(tmpl, args);
		}
	}
	else
	{
		// `template<> int digits(unsigned int)`: the specialized
		// arguments deduce from the declared signature (14.7.3p11).
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			inner.declarator.get(), specs.type);
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
	if (spec->body_emitted)
		throw runtime_error("redefinition of specialization " +
		                    spec->name);
	spec->explicit_def = &inner;
	spec->explicit_inline = specs.is_inline;
	// A definition emits at its point of declaration (strong unless
	// inline); uses before it deduced only the signature.
	if (inner.body)
		InstantiateFunctionBody(*spec->owner, *spec);
}

// --- variable templates -------------------------------------------------------

void SemBinder::CaptureVariableTemplate(const AstDecl& decl,
                                        const AstDecl& inner)
{
	const AstName* id = inner.declarators[0].declarator->IdName();
	const string& name = id->parts[0].identifier;
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	ScopeBinding* existing = FindOwnBinding(*current_, name);
	TemplateInfo* tmpl = 0;
	if (existing)
	{
		if (existing->kind != SB_VARIABLE_TEMPLATE)
			throw runtime_error(name +
			                    " redeclared as a variable template");
		tmpl = existing->templ;
	}
	else
	{
		tmpl = unit_.templates.Create(TMPL_VARIABLE, name, current_);
		ScopeBinding fresh;
		fresh.kind = SB_VARIABLE_TEMPLATE;
		fresh.name = name;
		fresh.access = current_access_;
		fresh.templ = tmpl;
		AddBinding(*current_, fresh);
	}
	tmpl->params = params;
	tmpl->var_decl = &inner;
	const AstInitializer* init = inner.declarators[0].init.get();
	if (init && init->kind == INIT_EQ && init->expr)
		tmpl->var_init = init->expr.get();
}

void SemBinder::RegisterVariablePartial(const AstDecl& decl,
                                        const AstDecl& inner)
{
	const AstName* id = inner.declarators[0].declarator->IdName();
	const AstNamePart& part = id->parts.back();
	const ScopeBinding* found =
		UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found || found->kind != SB_VARIABLE_TEMPLATE || !found->templ)
		throw runtime_error(part.identifier +
		                    " does not name a variable template");
	TemplateInfo& tmpl = *found->templ;
	PartialSpecialization partial;
	CollectTemplateParams(decl, partial.params);
	partial.pattern = ComposePartialPattern(tmpl, partial.params, part);
	const AstInitializer* init = inner.declarators[0].init.get();
	if (init && init->kind == INIT_EQ && init->expr)
		partial.init = init->expr.get();
	tmpl.partials.push_back(partial);
}

void SemBinder::BindVariableExplicitSpecialization(const AstDecl& inner)
{
	if (inner.declarators.size() != 1 || !inner.declarators[0].declarator)
		throw OutsideBoundary("explicit specialization declarator");
	const AstName* id = inner.declarators[0].declarator->IdName();
	if (!id || id->parts.size() != 1 ||
	    id->parts.back().kind != NP_TEMPLATE_ID)
		throw OutsideBoundary("explicit specialization declarator");
	const AstNamePart& part = id->parts.back();
	const ScopeBinding* found =
		UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found || found->kind != SB_VARIABLE_TEMPLATE || !found->templ)
		throw runtime_error(part.identifier +
		                    " does not name a variable template");
	TemplateInfo& tmpl = *found->templ;
	vector<TemplateArg> args = ResolveTemplateArgumentList(tmpl, part);
	const AstInitializer* init = inner.declarators[0].init.get();
	if (!init || init->kind != INIT_EQ || !init->expr)
		throw OutsideBoundary("variable specialization initializer");
	tmpl.var_explicit[TemplateArgumentKey(args)] = init->expr.get();
}

// A use of `name<args>`: the per-key objectless constant binding,
// evaluated on first demand from the selected initializer (explicit
// specialization, matching partial, or the primary).
const ScopeBinding* SemBinder::ResolveVariableTemplateId(
	TemplateInfo& tmpl, const AstNamePart& part)
{
	vector<TemplateArg> args = ResolveTemplateArgumentList(tmpl, part);
	for (size_t i = 0; i < args.size(); i++)
		if (TemplateArgIsDependent(args[i]))
			throw OutsideBoundary("dependent variable-template use");
	string key = TemplateArgumentKey(args);
	unique_ptr<ScopeBinding>& slot = tmpl.var_specs[key];
	if (slot)
		return slot.get();
	const AstExpr* init = 0;
	Scope* alias = 0;
	map<string, const AstExpr*>::const_iterator explicit_it =
		tmpl.var_explicit.find(key);
	if (explicit_it != tmpl.var_explicit.end())
	{
		init = explicit_it->second;
		alias = MakeArgumentAliasScope(tmpl, args);
	}
	else
	{
		vector<TemplateArg> bound;
		int partial = MatchPartialSpecialization(tmpl, args, bound);
		if (partial >= 0)
		{
			init = tmpl.partials[partial].init;
			TemplateInfo shadow;
			shadow.params = tmpl.partials[partial].params;
			shadow.declaring = tmpl.declaring;
			alias = MakeArgumentAliasScope(shadow, bound);
		}
		else
		{
			init = tmpl.var_init;
			alias = MakeArgumentAliasScope(tmpl, args);
		}
	}
	if (!init || !tmpl.var_decl)
		throw runtime_error("variable template " + tmpl.name +
		                    " has no usable initializer");
	Scope* saved = current_;
	current_ = alias;
	ConstValue value;
	TypePtr declared;
	try
	{
		value = EvaluateConstExpr(*init, *this);
		DeclSpecifierInfo specs = builder_.ProcessSpecifiers(
			tmpl.var_decl->specifiers, true);
		declared = specs.type;
		if (specs.is_constexpr)
			declared = MakeCvQualifiedType(declared, true, false);
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	if (!IsIntegralType(RemoveTopCv(declared)) &&
	    RemoveTopCv(declared)->kind != TK_ENUM)
		throw OutsideBoundary("non-integral variable template");
	TypePtr bare = RemoveTopCv(declared);
	value = ConvertConstValue(
		value, bare->kind == TK_ENUM ? bare->named->enum_underlying
		                             : bare->fundamental);
	slot.reset(new ScopeBinding());
	slot->kind = SB_VARIABLE;
	slot->name = tmpl.name + TemplateArgumentSpelling(args);
	slot->type = MakeCvQualifiedType(bare, true, false);
	slot->has_value = true;
	slot->no_object = true;
	slot->value = value;
	slot->owner = tmpl.declaring;
	slot->home = tmpl.declaring;
	return slot.get();
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
	partial.decl = &inner;
	tmpl.partials.push_back(partial);
}

void SemBinder::InstantiateClassFromPartial(
	TemplateInfo& tmpl, ClassSpecialization& spec,
	const PartialSpecialization& partial,
	const vector<TemplateArg>& bound)
{
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	spec.instantiated = true;
	spec.from_partial = true;
	TemplateInfo shadow;
	shadow.params = partial.params;
	shadow.declaring = tmpl.declaring;
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
		throw;
	}
	allow_qualified_class_name_ = saved_allow;
	if (Scope* members = model_.MemberScope(spec.entity))
		members->name = spec.entity->name;
}
