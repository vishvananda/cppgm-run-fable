#include "sema/sem_instantiation.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA19 assignment boundary");
}

}  // namespace

// PA21+ variable templates: pattern capture, partial-specialization
// registration, explicit specializations, and template-id resolution
// producing the instantiated variable binding. Split from
// sem_spec.cpp, which keeps the class/function explicit-specialization
// dispatch.

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

// PA23: a class-typed variable-template use materializes a weak
// object (the constexpr image emits statically; reads designate the
// storage). Runs with `current_` already inside the argument alias
// scope; the cache slot fills before the initializer analysis so
// self-references terminate.
const ScopeBinding* SemBinder::ResolveClassVariableTemplate(
	TemplateInfo& tmpl, const vector<TemplateArg>& args,
	const string& key, const TypePtr& declared, const AstExpr& init,
	bool is_constexpr)
{
	unique_ptr<ScopeBinding>& slot = tmpl.var_specs[key];
	slot.reset(new ScopeBinding());
	ScopeBinding fresh;
	fresh.kind = SB_VARIABLE;
	fresh.name = tmpl.name + TemplateArgumentSpelling(args);
	fresh.type = MakeCvQualifiedType(RemoveTopCv(declared), true,
	                                 false);
	fresh.owner = tmpl.declaring;
	fresh.home = tmpl.declaring;
	fresh.var_spec_template = &tmpl;
	fresh.var_spec_args = args;
	// The definition is a unit-level object regardless of where the
	// first use sits (a body-analysis use must not make it a local).
	unit_.items.push_back(MakeSemNode(SN_VARIABLE));
	SemNode* item = unit_.items.back().get();
	item->name = QualifiedScopePath(tmpl.declaring) + fresh.name;
	item->type = fresh.type;
	item->entity_scope = tmpl.declaring;
	item->entity_name = fresh.name;
	item->has_explicit_init = true;
	item->weak_def = true;
	// The binding completes locally before the scope registration:
	// the initializer analysis below may add bindings to the declaring
	// scope itself (another class-typed variable template), so no
	// reference into its binding vector survives the analysis.
	SemValue value = analyzer_.Analyze(init);
	analyzer_.CopyInitialize(value, fresh.type, "initialization");
	item->children.push_back(std::move(value.node));
	FinishConstexprObject(*item, fresh, is_constexpr);
	// The scope registration makes the entity resolvable by
	// (scope, name) like an ordinary object (the lowering identity).
	AddBinding(*tmpl.declaring, fresh);
	*slot = fresh;
	return slot.get();
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
			shadow.capture_seq = tmpl.capture_seq;
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
	// The evaluation context serves this one resolution; the cached
	// slot records the declaring scope.
	TransientScope alias_release(model_, &alias);
	Scope* saved = current_;
	current_ = alias;
	ConstValue value;
	TypePtr declared;
	bool is_constexpr = false;
	try
	{
		DeclSpecifierInfo specs = builder_.ProcessSpecifiers(
			tmpl.var_decl->specifiers, true);
		declared = specs.type;
		is_constexpr = specs.is_constexpr;
		if (specs.is_constexpr)
			declared = MakeCvQualifiedType(declared, true, false);
		// PA23: a class-typed variable template gets storage - the
		// constexpr image emits weak and reads designate the object.
		if (RemoveTopCv(declared)->kind == TK_CLASS)
		{
			const ScopeBinding* object = ResolveClassVariableTemplate(
				tmpl, args, key, declared, *init, is_constexpr);
			current_ = saved;
			return object;
		}
		try
		{
			value = EvaluateConstExpr(*init, *this);
		}
		catch (...)
		{
			// Outside the PA11 subset: the full PA20 engine evaluates
			// the analyzed initializer (builtin folds, constexpr
			// calls).
			if (!TryFullConstant(*init, value))
				throw;
		}
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

