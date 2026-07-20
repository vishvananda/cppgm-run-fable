#include "sema/sem_instantiation.h"

#include <set>
#include <stdexcept>

#include "ast/ast_text.h"
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
			// template; a template-id declarator is an in-class
			// partial specialization of it (which must not overwrite
			// the primary's parameter list).
			if (id->parts.back().kind == NP_TEMPLATE_ID)
				RegisterVariablePartial(decl, inner);
			else
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
	if (id && id->parts.size() == 1 &&
	    id->parts[0].kind == NP_CONVERSION_FUNCTION)
	{
		CaptureConversionFunctionTemplate(decl, inner, *cls);
		return;
	}
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

// A conversion-function template (14.8.2.3): recorded on the
// ClassInfo; conversion classification deduces it against the
// required destination type and synthesizes an ordinary
// ClassConversion entry per deduced specialization.
void SemBinder::CaptureConversionFunctionTemplate(const AstDecl& decl,
                                                  const AstDecl& inner,
                                                  ClassInfo& cls)
{
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	TemplateInfo* tmpl = unit_.templates.Create(
		TMPL_FUNCTION, "operator @conversion", current_);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition =
		inner.kind == DK_SPECIAL_MEMBER_DEFINITION && inner.body;
	tmpl->member_of = cls.entity;
	tmpl->member_access = current_access_;
	tmpl->member_pattern = &decl;
	const AstName* id = inner.declarator->IdName();
	tmpl->conversion_type = id->parts.back().conversion_type.get();
	cls.conversion_templates.push_back(tmpl);
}

// --- friend templates -----------------------------------------------------

namespace {

bool FriendTypeIdDepends(const AstTypeId& id, const std::set<string>& outer,
                         const string& cls_name);
bool FriendExprDepends(const AstExpr& expr, const std::set<string>& outer,
                       const string& cls_name);

// A bare identifier equal to an outer parameter (any kind) or to the
// enclosing template's own (injected) name re-resolves per
// instantiation; the class name used as `name<args>` does not - the
// arguments decide, and they are walked on their own.
bool FriendNameDepends(const AstName& name, const std::set<string>& outer,
                       const string& cls_name)
{
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if (outer.count(part.identifier))
			return true;
		if (part.kind == NP_IDENTIFIER && part.identifier == cls_name)
			return true;
		for (size_t a = 0; a < part.arguments.size(); a++)
		{
			const AstTemplateArgument& argument = part.arguments[a];
			if (argument.type &&
			    FriendTypeIdDepends(*argument.type, outer, cls_name))
				return true;
			if (argument.expr &&
			    FriendExprDepends(*argument.expr, outer, cls_name))
				return true;
		}
		if (part.conversion_type &&
		    FriendTypeIdDepends(*part.conversion_type, outer, cls_name))
			return true;
	}
	return false;
}

bool FriendSpecifiersDepend(const AstSpecifierSeq& seq,
                            const std::set<string>& outer,
                            const string& cls_name)
{
	for (size_t i = 0; i < seq.size(); i++)
	{
		if (seq[i].kind == SPEC_TYPE_NAME &&
		    FriendNameDepends(seq[i].name, outer, cls_name))
			return true;
		if (seq[i].kind == SPEC_DECLTYPE && seq[i].decltype_expr &&
		    FriendExprDepends(*seq[i].decltype_expr, outer, cls_name))
			return true;
	}
	return false;
}

bool FriendDeclaratorDepends(const AstDeclarator& declarator,
                             const std::set<string>& outer,
                             const string& cls_name)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		switch (item.kind)
		{
		case DI_MEMBER_PTR:
			if (FriendNameDepends(item.name, outer, cls_name))
				return true;
			break;
		case DI_NESTED:
			if (item.nested &&
			    FriendDeclaratorDepends(*item.nested, outer, cls_name))
				return true;
			break;
		case DI_PARAMS:
			if (!item.params)
				break;
			for (size_t p = 0; p < item.params->parameters.size(); p++)
			{
				const AstParameter& parameter =
					item.params->parameters[p];
				if (FriendSpecifiersDepend(parameter.specifiers, outer,
				                           cls_name))
					return true;
				if (parameter.declarator &&
				    FriendDeclaratorDepends(*parameter.declarator,
				                            outer, cls_name))
					return true;
			}
			break;
		case DI_TRAILING_RETURN:
			if (item.trailing_type &&
			    FriendTypeIdDepends(*item.trailing_type, outer,
			                        cls_name))
				return true;
			break;
		case DI_ARRAY:
			if (item.array_bound &&
			    FriendExprDepends(*item.array_bound, outer, cls_name))
				return true;
			break;
		default:
			break;
		}
	}
	return false;
}

bool FriendTypeIdDepends(const AstTypeId& id, const std::set<string>& outer,
                         const string& cls_name)
{
	if (FriendSpecifiersDepend(id.specifiers, outer, cls_name))
		return true;
	return id.declarator &&
		FriendDeclaratorDepends(*id.declarator, outer, cls_name);
}

bool FriendExprDepends(const AstExpr& expr, const std::set<string>& outer,
                       const string& cls_name)
{
	if (FriendNameDepends(expr.name, outer, cls_name))
		return true;
	if (expr.type && FriendTypeIdDepends(*expr.type, outer, cls_name))
		return true;
	for (size_t i = 0; i < expr.operands.size(); i++)
		if (expr.operands[i] &&
		    FriendExprDepends(*expr.operands[i], outer, cls_name))
			return true;
	for (size_t i = 0; i < expr.arguments.size(); i++)
		if (expr.arguments[i] &&
		    FriendExprDepends(*expr.arguments[i], outer, cls_name))
			return true;
	return false;
}

// Whether a friend template's written declaration depends on the
// enclosing instantiation (an outer parameter or the bare
// injected-class-name anywhere in its signature or its own parameter
// list). Dependent friends are distinct per instantiation (14.5.4);
// independent ones name a single namespace-scope template shared by
// every instantiation and by ordinary redeclarations.
bool FriendDeclarationDepends(const AstDecl& decl, const AstDecl& inner,
                              const std::set<string>& outer,
                              const string& cls_name)
{
	for (size_t i = 0; i < decl.template_params.size(); i++)
	{
		const AstTemplateParameter& parameter = decl.template_params[i];
		if (parameter.default_type &&
		    FriendTypeIdDepends(*parameter.default_type, outer, cls_name))
			return true;
		if (parameter.default_expr &&
		    FriendExprDepends(*parameter.default_expr, outer, cls_name))
			return true;
		if (FriendSpecifiersDepend(parameter.specifiers, outer, cls_name))
			return true;
		if (parameter.declarator &&
		    FriendDeclaratorDepends(*parameter.declarator, outer,
		                            cls_name))
			return true;
	}
	if (FriendSpecifiersDepend(inner.specifiers, outer, cls_name))
		return true;
	const AstDeclarator* declarator = 0;
	if (inner.kind == DK_FUNCTION)
		declarator = inner.declarator.get();
	else if (inner.declarators.size() == 1)
		declarator = inner.declarators[0].declarator.get();
	return declarator &&
		FriendDeclaratorDepends(*declarator, outer, cls_name);
}

}  // namespace

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
			const AstDecl* forward = 0;
			if (inner.kind == DK_CLASS_FORWARD)
			{
				name = &inner.class_name;
				forward = &inner;
			}
			else
				for (size_t i = 0; i < inner.specifiers.size(); i++)
					if (inner.specifiers[i].kind == SPEC_NESTED_DECL &&
					    inner.specifiers[i].nested_decl)
					{
						name = &inner.specifiers[i].nested_decl->class_name;
						forward =
							inner.specifiers[i].nested_decl.get();
					}
			if (name && name->IsPlainIdentifier())
			{
				const ScopeBinding* found = UnqualifiedLookup(
					current_, name->parts[0].identifier, SLF_ANY);
				// 11.3p1: a first-declaration friend template also
				// declares the class template in the enclosing
				// namespace.
				if (!found && forward &&
				    (forward->kind == DK_CLASS_FORWARD ||
				     forward->kind == DK_CLASS))
				{
					CaptureClassTemplate(decl, *forward, false);
					found = UnqualifiedLookup(
						current_, name->parts[0].identifier, SLF_ANY);
				}
				if (found && found->kind == SB_CLASS_TEMPLATE &&
				    found->templ && found->templ->anchor)
					cls->friend_classes.push_back(found->templ->anchor);
			}
			else if (name && !name->parts.empty() &&
			         name->parts.back().kind == NP_IDENTIFIER)
			{
				// A qualified friend template (`template<class...>
				// friend struct __detail::_Map_base;`) references the
				// already-declared template; the anchor grants every
				// specialization (11.3p1).
				const ScopeBinding* found = 0;
				try
				{
					if (Scope* prefix = ResolvePrefixScope(*name))
						found = QualifiedLookup(
							*prefix, name->parts.back().identifier,
							SLF_ANY);
				}
				catch (const std::exception&)
				{
					found = 0;
				}
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
	// declaration appears; ADL finds it (7.3.1.2p3). A friend whose
	// written signature depends on the enclosing instantiation (outer
	// parameters, the bare injected-class-name) is a distinct template
	// per instantiation (14.5.4) and merges only within its own; an
	// independent signature names one shared namespace-scope template.
	Scope* inst = cls->members && cls->members->parent &&
	              cls->members->parent->kind == SCOPE_TEMPLATE_PARAMS
	                  ? cls->members->parent : 0;
	bool dependent_friend = false;
	if (inst)
	{
		string cls_name = cls->entity && cls->entity->spec_template
			? cls->entity->spec_template->name
			: (cls->entity ? cls->entity->name : string());
		// The alias scope also injects the class's own name; only its
		// bare (injected-class-name) uses count, which the walk
		// decides itself.
		std::set<string> outer;
		for (size_t i = 0; i < inst->bindings.size(); i++)
			if (inst->bindings[i].name != cls_name)
				outer.insert(inst->bindings[i].name);
		dependent_friend =
			FriendDeclarationDepends(decl, inner, outer, cls_name);
	}
	Scope* saved = current_;
	current_ = ns;
	TemplateInfo* tmpl = 0;
	try
	{
		tmpl = CaptureFunctionTemplate(decl, inner, true,
		                               dependent_friend ? inst : ns);
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	if (tmpl)
	{
		// A dependent friend keeps the instantiation's argument
		// aliases visible to its lazily-composed pattern (an outer
		// `T` or the injected name in the signature).
		if (tmpl->declaring == ns && dependent_friend)
			tmpl->declaring = inst;
		// 11.3/3.4.1: the friend's signature resolves names with the
		// declaring class in scope (`prop::convertible` parameters);
		// its namespace identity stays `declaring`.
		if (!tmpl->lookup_scope && cls->members)
			tmpl->lookup_scope = cls->members;
		cls->friend_functions.push_back(
			std::make_pair(ns, tmpl->name));
	}
}

// --- out-of-class member-template definitions ------------------------------

// The canonical positional spelling of a conversion-type-id: a
// conversion template's identity rides this id (every conversion
// signature composes as `void() cv`), so definition pairing compares
// it with the parameter names positionalized (14.5.6.1).
static string ConversionTypeIdSpelling(const AstTypeId& id,
                                       const vector<TemplateParam>& params)
{
	string text = FlattenSpecifierSeq(id.specifiers);
	if (id.declarator)
		text += " | " + FlattenDeclarator(*id.declarator);
	return PositionalizeTemplateNames(text, params);
}

// A member-template definition whose qualifier resolved to a concrete
// class (or instantiated specialization) member scope: capture/merge
// there, pairing with the in-class declaration.
void SemBinder::CaptureQualifiedMemberTemplate(const AstDecl& decl,
                                               const AstDecl& inner,
                                               Scope* declaring,
                                               bool replace_instantiated)
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
			TemplateInfo* tmpl = CaptureFunctionTemplate(
				decl, inner, false, 0, replace_instantiated);
			if (tmpl)
			{
				tmpl->member_of = model_.ScopeEntity(declaring);
				if (!tmpl->member_pattern)
					tmpl->member_pattern = &decl;
				// PA36: an out-of-class definition replayed under an
				// argument-alias scope (renamed enclosing parameters,
				// 9.3p5) keeps resolving its spellings there.
				if (inner.kind == DK_FUNCTION && !tmpl->lookup_scope &&
				    saved && saved->kind == SCOPE_TEMPLATE_PARAMS)
					tmpl->lookup_scope = saved;
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
			const AstName* sm_id =
				inner.declarator ? inner.declarator->IdName() : 0;
			bool conversion = sm_id && !sm_id->parts.empty() &&
				sm_id->parts.back().kind == NP_CONVERSION_FUNCTION;
			vector<TemplateInfo*>& pool = conversion
				? cls->conversion_templates : cls->ctor_templates;
			// Pairing prefers a signature match among the same-arity
			// definition-less declarations (a conversion template's
			// identity is its conversion-type-id; a constructor
			// template's is its parameter clause). A lone same-arity
			// candidate keeps the tolerant match: its head may spell
			// types through class typedefs the comparison cannot chase.
			TemplateInfo* merged = 0;
			TemplateInfo* lone = 0;
			size_t open = 0;
			for (size_t i = 0; i < pool.size(); i++)
			{
				TemplateInfo& cand = *pool[i];
				if (cand.params.size() != params.size() ||
				    cand.has_definition || !cand.pattern_decl)
					continue;
				lone = &cand;
				open++;
				if (merged || !SameTemplateParameterKinds(params,
				                                          cand.params))
					continue;
				if (conversion)
				{
					if (cand.conversion_type &&
					    sm_id->parts.back().conversion_type &&
					    ConversionTypeIdSpelling(
					        *sm_id->parts.back().conversion_type,
					        params) ==
					        ConversionTypeIdSpelling(
					            *cand.conversion_type, cand.params))
						merged = &cand;
				}
				else if (SameFunctionTemplateSignature(cand, decl, inner,
				                                       false))
					merged = &cand;
			}
			if (!merged && open == 1)
				merged = lone;
			if (!merged)
				throw runtime_error("special member template "
				                    "definition matches no declaration");
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
	// 14.1p10: the in-class declaration's default arguments carry over
	// (the definition's parameter names win positionally).
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].kind != tmpl->params[i].kind)
			throw runtime_error("template parameter kind of " + name +
			                    " disagrees with its declaration");
		if (!params[i].default_type)
			params[i].default_type = tmpl->params[i].default_type;
		if (!params[i].default_expr)
			params[i].default_expr = tmpl->params[i].default_expr;
	}
	if (inner.kind != DK_CLASS)
	{
		tmpl->params = params;
		return;
	}
	if (tmpl->has_definition)
		throw runtime_error("redefinition of member class template " +
		                    name);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition = true;
	// Specializations named while only the declaration was visible
	// stay dormant (14.7.1p1): the first completeness demand
	// instantiates them through the ordinary body path.
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
	// The composed parameter names carry into forwarding syntheses
	// (pack elements keep the `name`, `name__pack2`, ... spellings);
	// an unnamed composition falls back to the declared spellings.
	bool any_named = false;
	for (size_t i = 0; i < spec->param_names.size(); i++)
		if (!spec->param_names[i].empty())
			any_named = true;
	if (any_named)
		ctor.param_names = spec->param_names;
	else if (tmpl.pattern_decl && tmpl.pattern_decl->declarator)
		CollectDeclaredParamNames(*tmpl.pattern_decl->declarator,
		                          ctor.param_names);
	// 12.8p2 last sentence: a constructor template never declares a
	// copy/move constructor, so the entry stays CK_ORDINARY.
	ctor.kind = CK_ORDINARY;
	if (tmpl.pattern_decl && tmpl.has_definition)
		ctor.definition = tmpl.pattern_decl;
	// 12.9p8: an inherited constructor template's entry forwards to
	// the base subobject's constructor instead of binding the pattern
	// body as its own.
	if (tmpl.member_of && cls.entity && tmpl.member_of != cls.entity)
	{
		ctor.inherited_base = tmpl.member_of;
		ctor.inherited_built = false;
		ctor.definition = 0;
	}
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
	// An inherited entry forwards through EnsureInheritedCtor; the
	// pattern body belongs to the base class.
	if (ctor.inherited_base)
		return;
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
	// The capture scope served the signature only; the body bind must
	// not leak nested pattern-composition names here.
	param_capture_scope_ = 0;
	DeferredBody body;
	body.decl = &inner;
	body.composed = composed;
	body.name = cls.entity->name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.member_template = true;
	AnalyzeDeferredBody(body);
	if (body.cls->ctors[index].unwind_no)
		mutable_spec.self.fn_unwind_no[0] = true;
}
