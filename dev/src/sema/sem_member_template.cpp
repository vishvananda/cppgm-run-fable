#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA21 member templates: capture of template declarations inside
// class bodies (member function templates, constructor templates,
// member class/alias/variable templates, friend templates) and the
// out-of-class member-template definition path. A member template of
// a class template is captured per enclosing specialization: the
// enclosing instantiation re-walks the member declaration with the
// outer arguments aliased, so each specialization owns fresh member
// TemplateInfos whose outer context is already concrete.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA21 assignment boundary");
}

bool SpecifiersSpell(const AstSpecifierSeq& specifiers, ETokenType keyword)
{
	for (size_t i = 0; i < specifiers.size(); i++)
		if (specifiers[i].kind == SPEC_KEYWORD &&
		    specifiers[i].keyword == keyword)
			return true;
	return false;
}

// Whether a declarator carries a parameter clause (declares a
// function) at any nesting level.
bool DeclaratorDeclaresFunction(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return true;
		if (item.kind == DI_NESTED && item.nested &&
		    DeclaratorDeclaresFunction(*item.nested))
			return true;
	}
	return false;
}

}  // namespace

// --- class-scope capture ------------------------------------------------

void SemBinder::BindMemberTemplateDeclaration(const AstDecl& decl,
                                              const AstDecl& inner)
{
	// Friend templates declare into the enclosing namespace (11.3p6).
	if ((inner.kind == DK_FUNCTION || inner.kind == DK_SIMPLE ||
	     inner.kind == DK_CLASS_FORWARD) &&
	    SpecifiersSpell(inner.specifiers, KW_FRIEND))
	{
		BindFriendTemplate(decl, inner);
		return;
	}
	switch (inner.kind)
	{
	case DK_CLASS:
	case DK_CLASS_FORWARD:
		// A member class template captures like a namespace-scope one;
		// its declaring scope is the class member scope. A template-id
		// name declares an in-class partial specialization.
		if (!inner.has_name)
			throw OutsideBoundary("member class template name");
		if (inner.class_name.parts.size() == 1 &&
		    inner.class_name.parts.back().kind == NP_TEMPLATE_ID)
		{
			RegisterClassPartial(decl, inner);
			return;
		}
		if (!inner.class_name.IsPlainIdentifier())
			throw OutsideBoundary("member class template name");
		CaptureClassTemplate(decl, inner, inner.kind == DK_CLASS);
		return;
	case DK_ALIAS:
		CaptureAliasTemplate(decl, inner);
		return;
	case DK_SPECIAL_MEMBER_DECLARATION:
	case DK_SPECIAL_MEMBER_DEFINITION:
		CaptureConstructorTemplate(decl, inner);
		return;
	case DK_FUNCTION:
	case DK_SIMPLE:
	{
		const AstName* id = 0;
		const AstDeclarator* declarator = 0;
		if (inner.kind == DK_FUNCTION)
			declarator = inner.declarator.get();
		else if (inner.declarators.size() == 1)
			declarator = inner.declarators[0].declarator.get();
		if (declarator)
			id = declarator->IdName();
		if (!id || id->parts.empty())
			throw OutsideBoundary("member template declarator");
		if (id->parts.size() > 1)
			throw OutsideBoundary("qualified member template "
			                      "declarator");
		if (inner.kind == DK_SIMPLE &&
		    !DeclaratorDeclaresFunction(*declarator))
		{
			// A static data member template is a class-scope variable
			// template.
			CaptureVariableTemplate(decl, inner);
			return;
		}
		CaptureMemberFunctionTemplate(decl, inner);
		return;
	}
	default:
		throw OutsideBoundary("member template form");
	}
}

void SemBinder::CaptureMemberFunctionTemplate(const AstDecl& decl,
                                              const AstDecl& inner)
{
	TemplateInfo* tmpl = CaptureFunctionTemplate(decl, inner);
	if (!tmpl)
		return;
	tmpl->member_of = model_.ScopeEntity(current_);
	tmpl->member_static = SpecifiersSpell(inner.specifiers, KW_STATIC);
	tmpl->member_access = current_access_;
	tmpl->member_pattern = &decl;
}

// A constructor template: recorded on the ClassInfo; construction
// sites deduce it against their argument lists and synthesize an
// ordinary ClassCtor entry per selected specialization.
void SemBinder::CaptureConstructorTemplate(const AstDecl& decl,
                                           const AstDecl& inner)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw OutsideBoundary("constructor template scope");
	const AstName* id = inner.declarator ? inner.declarator->IdName() : 0;
	if (!id || !id->IsPlainIdentifier())
		throw OutsideBoundary("special member template form");
	if (id->parts.back().tilde)
		throw runtime_error("destructor template is ill-formed");
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	TemplateInfo* tmpl = unit_.templates.Create(
		TMPL_FUNCTION, id->parts[0].identifier, current_);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition =
		inner.kind == DK_SPECIAL_MEMBER_DEFINITION && inner.body;
	tmpl->member_of = cls->entity;
	tmpl->member_access = current_access_;
	tmpl->member_pattern = &decl;
	cls->ctor_templates.push_back(tmpl);
	// 8.5.1p1/12.1: a declared constructor template makes the class
	// non-aggregate with user-provided construction.
	cls->is_aggregate = false;
	cls->has_user_ctor = true;
}

// --- friend templates -----------------------------------------------------

void SemBinder::BindFriendTemplate(const AstDecl& decl,
                                   const AstDecl& inner)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw runtime_error("friend template outside a class");
	// `template<class U> friend class X;` befriends every
	// specialization of X (11.3p1); recording the template's anchor
	// entity grants the access.
	if (inner.kind == DK_CLASS_FORWARD ||
	    (inner.kind == DK_SIMPLE && inner.declarators.empty()))
	{
		Scope* saved = current_;
		current_ = EnclosingNamespace();
		try
		{
			const AstName* name = 0;
			if (inner.kind == DK_CLASS_FORWARD)
				name = &inner.class_name;
			else
				for (size_t i = 0; i < inner.specifiers.size(); i++)
					if (inner.specifiers[i].kind == SPEC_NESTED_DECL &&
					    inner.specifiers[i].nested_decl)
						name = &inner.specifiers[i].nested_decl->class_name;
			if (name && name->IsPlainIdentifier())
			{
				const ScopeBinding* found = UnqualifiedLookup(
					current_, name->parts[0].identifier, SLF_ANY);
				if (found && found->kind == SB_CLASS_TEMPLATE &&
				    found->templ && found->templ->anchor)
					cls->friend_classes.push_back(found->templ->anchor);
			}
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		return;
	}
	const AstName* id = 0;
	if (inner.kind == DK_FUNCTION && inner.declarator)
		id = inner.declarator->IdName();
	else if (inner.kind == DK_SIMPLE && inner.declarators.size() == 1 &&
	         inner.declarators[0].declarator)
		id = inner.declarators[0].declarator->IdName();
	if (!id || id->parts.empty())
		throw OutsideBoundary("friend template declarator");
	Scope* ns = EnclosingNamespace();
	if (id->parts.size() > 1)
	{
		// A qualified friend template names an already-declared
		// template; the declaration only grants access (11.3p10).
		Scope* declaring = ResolvePrefixScope(*id);
		cls->friend_functions.push_back(std::make_pair(
			declaring, DeclaredFunctionName(id->parts.back())));
		return;
	}
	// An unqualified friend template declares into the enclosing
	// namespace, hidden from ordinary lookup until a namespace-scope
	// declaration appears; ADL finds it (7.3.1.2p3). Each enclosing
	// instantiation re-captures; the same pattern merges silently.
	Scope* saved = current_;
	current_ = ns;
	TemplateInfo* tmpl = 0;
	try
	{
		tmpl = CaptureFunctionTemplate(decl, inner, true);
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	if (tmpl)
		cls->friend_functions.push_back(
			std::make_pair(ns, tmpl->name));
}

// --- out-of-class member-template definitions ------------------------------

// A member-template definition whose qualifier resolved to a concrete
// class (or instantiated specialization) member scope: capture/merge
// there, pairing with the in-class declaration.
void SemBinder::CaptureQualifiedMemberTemplate(const AstDecl& decl,
                                               const AstDecl& inner,
                                               Scope* declaring)
{
	Scope* saved = current_;
	current_ = declaring;
	try
	{
		switch (inner.kind)
		{
		case DK_CLASS:
		case DK_CLASS_FORWARD:
		{
			// The qualified class-name's terminal is the member
			// template's name; capture merges with the in-class
			// declaration through the ordinary redeclaration path. The
			// class-name must read as unqualified inside the member
			// scope, so the capture goes through the terminal name.
			CaptureQualifiedMemberClassTemplate(decl, inner);
			break;
		}
		case DK_FUNCTION:
		case DK_SIMPLE:
		{
			TemplateInfo* tmpl = CaptureFunctionTemplate(decl, inner);
			if (tmpl)
			{
				tmpl->member_of = model_.ScopeEntity(declaring);
				if (!tmpl->member_pattern)
					tmpl->member_pattern = &decl;
			}
			break;
		}
		case DK_SPECIAL_MEMBER_DEFINITION:
		case DK_SPECIAL_MEMBER_DECLARATION:
		{
			// An out-of-class constructor-template definition merges
			// with the in-class-captured constructor template.
			const NamedTypeInfo* entity = model_.ScopeEntity(declaring);
			ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
			if (!cls)
				throw runtime_error("constructor template of an "
				                    "unknown class");
			vector<TemplateParam> params;
			CollectTemplateParams(decl, params);
			TemplateInfo* merged = 0;
			for (size_t i = 0; i < cls->ctor_templates.size(); i++)
				if (cls->ctor_templates[i]->params.size() ==
				    params.size() && !cls->ctor_templates[i]->has_definition)
					merged = cls->ctor_templates[i];
			if (!merged)
				throw runtime_error("constructor template definition "
				                    "matches no declaration");
			merged->decl = &decl;
			merged->pattern_decl = &inner;
			merged->has_definition = inner.body != 0;
			break;
		}
		default:
			throw OutsideBoundary("qualified member template form");
		}
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
}

// The DK_CLASS leg of CaptureQualifiedMemberTemplate: the class-name
// is qualified, but capture keys on the terminal identifier in the
// (already swapped-in) member scope.
void SemBinder::CaptureQualifiedMemberClassTemplate(const AstDecl& decl,
                                                    const AstDecl& inner)
{
	const string& name = inner.class_name.parts.back().identifier;
	ScopeBinding* existing = FindOwnBinding(*current_, name);
	if (!existing || existing->kind != SB_CLASS_TEMPLATE ||
	    !existing->templ)
		throw runtime_error("member class template definition matches "
		                    "no declaration");
	TemplateInfo* tmpl = existing->templ;
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	if (tmpl->params.size() != params.size())
		throw runtime_error("template parameter list of " + name +
		                    " disagrees with its declaration");
	if (inner.kind != DK_CLASS)
		return;
	if (tmpl->has_definition)
		throw runtime_error("redefinition of member class template " +
		                    name);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition = true;
	// Specializations named while only the declaration was visible
	// upgrade now.
	for (map<string, unique_ptr<ClassSpecialization>>::iterator it =
	         tmpl->class_specs.begin();
	     it != tmpl->class_specs.end(); ++it)
		if (!it->second->instantiated)
			InstantiateClassSpecialization(*tmpl, *it->second);
}

// --- constructor templates at construction sites ---------------------------

void SemBinder::AppendCtorTemplateCandidates(const ClassInfo& cls,
                                             vector<SemValue>& args,
                                             vector<TypePtr>& candidates,
                                             vector<size_t>& min_arity,
                                             vector<size_t>& positions,
                                             vector<bool>* is_template)
{
	if (cls.ctor_templates.empty())
		return;
	ClassInfo& mutable_cls = const_cast<ClassInfo&>(cls);
	for (size_t t = 0; t < cls.ctor_templates.size(); t++)
	{
		const FunctionSpecialization* spec =
			DeduceFunctionTemplate(*cls.ctor_templates[t], args, 0);
		if (!spec)
			continue;
		int index = EnsureCtorTemplateEntry(mutable_cls, spec);
		if (index < 0)
			continue;
		bool present = false;
		for (size_t i = 0; i < positions.size(); i++)
			if (positions[i] == (size_t)index)
				present = true;
		if (present)
			continue;
		const ClassCtor& ctor = cls.ctors[index];
		size_t required = ctor.type->parameters.size();
		while (required > 0 && required <= ctor.defaults.size() &&
		       ctor.defaults[required - 1])
			required--;
		candidates.push_back(ctor.type);
		min_arity.push_back(required);
		positions.push_back((size_t)index);
		if (is_template)
			is_template->push_back(true);
	}
}

int SemBinder::EnsureCtorTemplateEntry(ClassInfo& cls,
                                       const FunctionSpecialization* spec)
{
	for (size_t i = 0; i < cls.ctors.size(); i++)
		if (cls.ctors[i].tmpl_spec == spec)
			return (int)i;
	TemplateInfo& tmpl = *spec->owner;
	ClassCtor ctor;
	ctor.type = spec->type;
	ctor.access = tmpl.member_access;
	ctor.tmpl_spec = spec;
	ctor.tmpl_param_scope = spec->param_scope;
	ctor.defaults = spec->self.fn_defaults[0];
	// 12.8p2 last sentence: a constructor template never declares a
	// copy/move constructor, so the entry stays CK_ORDINARY.
	ctor.kind = CK_ORDINARY;
	if (tmpl.pattern_decl && tmpl.has_definition)
		ctor.definition = tmpl.pattern_decl;
	cls.ctors.push_back(ctor);
	int index = (int)(cls.ctors.size() - 1);
	// 14.7.1p2: the body instantiates only when overload resolution
	// selects this candidate (ResolveClassConstructor), not when it
	// merely joins the candidate set.
	if (!tmpl.has_definition)
		const_cast<FunctionSpecialization*>(spec)->body_emitted = true;
	return index;
}

// The instantiated body of a selected constructor-template
// specialization: an ordinary deferred constructor body bound under
// the specialization's argument alias scope (weak, demand-emitted).
void SemBinder::InstantiateCtorTemplateBody(ClassInfo& cls, int index)
{
	ClassCtor& ctor = cls.ctors[index];
	const FunctionSpecialization* spec = ctor.tmpl_spec;
	if (!spec || !spec->owner || !spec->owner->pattern_decl)
		return;
	FunctionSpecialization& mutable_spec =
		*const_cast<FunctionSpecialization*>(spec);
	if (mutable_spec.body_emitted)
		return;
	mutable_spec.body_emitted = true;
	const AstDecl& inner = *spec->owner->pattern_decl;
	if (!inner.body)
		return;
	// Re-compose the declarator under a fresh function scope so the
	// parameters bind with their declared names.
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, cls.entity->name,
	                                     spec->param_scope);
	fn_scope->fn_type = spec->type;
	InstantiationContext context(*this, fn_scope, true);
	param_capture_scope_ = fn_scope;
	PreBindDeclaredParameters(inner.declarator.get());
	last_pack_param_ = PackParamRecord();
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		inner.declarator.get(), MakeFundamentalType(FT_VOID));
	BindCapturedPackParameter(fn_scope);
	DeferredBody body;
	body.decl = &inner;
	body.composed = composed;
	body.name = cls.entity->name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	AnalyzeDeferredBody(body);
	if (body.cls->ctors[index].unwind_no)
		mutable_spec.self.fn_unwind_no[0] = true;
}
