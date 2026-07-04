#include "sema/sem_binder.h"

#include <set>
#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA18 template machinery: capture of template declarations and
// on-demand instantiation. A template body is never analyzed at its
// declaration; instantiation re-binds the stored AST with the
// parameter names aliased to the concrete argument types, so the
// PA11-PA17 declaration/class/expression machinery does the semantic
// work unchanged. Instantiated definitions emit as weak
// (demand-emitted) LowIR, matching 14.7 linkage expectations.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA18 assignment boundary");
}

bool DeclaratorSpellsNoexcept(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
		if (declarator.items[i].kind == DI_FUNC_QUAL &&
		    declarator.items[i].qual.kind == FQ_NOEXCEPT)
			return true;
	return false;
}

size_t DeclaratorArity(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
		if (declarator.items[i].kind == DI_PARAMS &&
		    declarator.items[i].params)
			return declarator.items[i].params->parameters.size();
	return 0;
}

}  // namespace

// --- instantiation context --------------------------------------------------

SemBinder::InstantiationContext::InstantiationContext(SemBinder& binder,
                                                      Scope* scope,
                                                      bool instantiating)
	: binder_(binder)
{
	saved_scope_ = binder.current_;
	saved_fields_ = binder.current_fields_;
	saved_access_ = binder.current_access_;
	saved_c_linkage_ = binder.in_c_linkage_;
	saved_method_ = binder.method_;
	saved_return_ = binder.current_return_;
	saved_bit_field_ = binder.in_bit_field_;
	saved_instantiating_ = binder.instantiating_;
	saved_unevaluated_ = binder.in_unevaluated_operand_;
	saved_param_capture_ = binder.param_capture_scope_;
	saved_bf_units_.swap(binder.bf_units_written_);
	saved_parents_.swap(binder.parents_);
	saved_open_classes_.swap(binder.open_classes_);
	saved_deferred_.swap(binder.deferred_bodies_);
	binder.current_ = scope;
	binder.current_fields_ = 0;
	binder.current_access_ = MA_PUBLIC;
	binder.in_c_linkage_ = false;
	binder.method_ = MethodContext();
	binder.current_return_ = TypePtr();
	binder.in_bit_field_ = false;
	if (instantiating)
		binder.instantiating_ = true;
	// The instantiated declarations are their own evaluation context,
	// even when the instantiation triggered inside an unevaluated
	// operand.
	binder.in_unevaluated_operand_ = false;
	binder.param_capture_scope_ = 0;
	binder.instantiation_depth_++;
}

SemBinder::InstantiationContext::~InstantiationContext()
{
	binder_.instantiation_depth_--;
	binder_.current_ = saved_scope_;
	binder_.current_fields_ = saved_fields_;
	binder_.current_access_ = saved_access_;
	binder_.in_c_linkage_ = saved_c_linkage_;
	binder_.method_ = saved_method_;
	binder_.current_return_ = saved_return_;
	binder_.in_bit_field_ = saved_bit_field_;
	binder_.instantiating_ = saved_instantiating_;
	binder_.in_unevaluated_operand_ = saved_unevaluated_;
	binder_.param_capture_scope_ = saved_param_capture_;
	binder_.bf_units_written_.swap(saved_bf_units_);
	binder_.parents_.swap(saved_parents_);
	binder_.open_classes_.swap(saved_open_classes_);
	binder_.deferred_bodies_.swap(saved_deferred_);
}

// --- capture --------------------------------------------------------------

// The declared parameter names of a function declarator (empty
// entries for unnamed parameters).
void SemBinder::CollectDeclaredParamNames(const AstDeclarator& declarator,
                                          vector<string>& out)
{
	const AstParameterClause* clause = 0;
	for (size_t i = 0; i < declarator.items.size() && !clause; i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			clause = item.params.get();
		else if (item.kind == DI_NESTED && item.nested)
			for (size_t j = 0;
			     j < item.nested->items.size() && !clause; j++)
				if (item.nested->items[j].kind == DI_PARAMS)
					clause = item.nested->items[j].params.get();
	}
	if (!clause)
		return;
	for (size_t i = 0; i < clause->parameters.size(); i++)
	{
		string name;
		if (clause->parameters[i].declarator)
			if (const AstName* id =
			        clause->parameters[i].declarator->IdName())
				if (id->IsPlainIdentifier())
					name = id->parts[0].identifier;
		out.push_back(name);
	}
}

// Whether a declarator carries a parameter clause (declares a
// function) at any nesting level.
static bool DeclaratorHasParameterClause(const AstDeclarator& declarator)
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

void SemBinder::BindTemplateDeclaration(const AstDecl& decl)
{
	if (!decl.has_parameter_list)
	{
		BindExplicitSpecialization(decl);
		return;
	}
	if (!decl.inner)
		throw OutsideBoundary("template declaration form");
	const AstDecl& inner = *decl.inner;
	if (current_->kind == SCOPE_CLASS)
	{
		BindMemberTemplateDeclaration(decl, inner);
		return;
	}
	// Instantiation re-walks bind under argument alias scopes; only a
	// qualified (member-definition) form is meaningful there.
	if (current_->kind != SCOPE_NAMESPACE &&
	    current_->kind != SCOPE_TEMPLATE_PARAMS)
		throw OutsideBoundary("block-scope template");
	switch (inner.kind)
	{
	case DK_CLASS:
		// A qualified class-name defines a nested class of a class
		// template out of class; a template-id is a partial
		// specialization (out of scope).
		if (!inner.has_name)
			throw OutsideBoundary("unnamed class template");
		if (inner.class_name.IsPlainIdentifier())
		{
			CaptureClassTemplate(decl, inner, true);
			return;
		}
		if (inner.class_name.parts.size() > 1)
		{
			// A namespace-qualified plain name defines a forward
			// -declared class template in its home namespace; a
			// template-id prefix defines a member of a class template.
			if (inner.class_name.parts.back().kind == NP_IDENTIFIER &&
			    QualifierIsNamespacePath(inner.class_name))
			{
				Scope* declaring = ResolvePrefixScope(inner.class_name);
				if (declaring->kind != SCOPE_NAMESPACE)
					throw OutsideBoundary("qualified template "
					                      "definition scope");
				Scope* saved = current_;
				current_ = declaring;
				try
				{
					CaptureQualifiedClassTemplate(decl, inner);
				}
				catch (...)
				{
					current_ = saved;
					throw;
				}
				current_ = saved;
				return;
			}
			// PA21: a prefix resolving to a concrete class (or
			// instantiated specialization) scope defines a member
			// class template - or one of its partial specializations
			// (template-id terminal) - of that class out of class.
			if (!TemplateMemberOwnerIsPattern(decl, inner.class_name))
			{
				Scope* declaring = ResolvePrefixScope(inner.class_name);
				if (declaring->kind == SCOPE_CLASS)
				{
					if (inner.class_name.parts.back().kind ==
					    NP_TEMPLATE_ID)
					{
						Scope* saved = current_;
						current_ = declaring;
						try
						{
							RegisterClassPartial(decl, inner);
						}
						catch (...)
						{
							current_ = saved;
							throw;
						}
						current_ = saved;
						return;
					}
					CaptureQualifiedMemberTemplate(decl, inner,
					                               declaring);
					return;
				}
			}
			RegisterTemplateMember(decl, inner.class_name);
			return;
		}
		// A single-part template-id class-name: a partial
		// specialization of a visible class template.
		RegisterClassPartial(decl, inner);
		return;
	case DK_CLASS_FORWARD:
		// PA21: a template-id forward declaration declares a partial
		// specialization (its definition completes the pattern).
		if (inner.class_name.parts.size() == 1 &&
		    inner.class_name.parts.back().kind == NP_TEMPLATE_ID)
		{
			RegisterClassPartial(decl, inner);
			return;
		}
		if (!inner.class_name.IsPlainIdentifier())
			throw OutsideBoundary("class template partial "
			                      "specialization");
		CaptureClassTemplate(decl, inner, false);
		return;
	case DK_FUNCTION:
	case DK_SIMPLE:
	{
		const AstName* id = 0;
		if (inner.kind == DK_FUNCTION)
			id = inner.declarator ? inner.declarator->IdName() : 0;
		else if (inner.declarators.size() == 1 &&
		         inner.declarators[0].declarator)
			id = inner.declarators[0].declarator->IdName();
		if (!id || id->parts.empty())
			throw OutsideBoundary("template declarator form");
		// PA19 variable templates: a plain declarator captures the
		// primary; a template-id declarator registers a partial
		// specialization. A qualified declarator is a class-template
		// member definition instead.
		if (inner.kind == DK_SIMPLE && id->parts.size() == 1 &&
		    !DeclaratorHasParameterClause(
		        *inner.declarators[0].declarator))
		{
			if (id->parts.back().kind == NP_TEMPLATE_ID)
				RegisterVariablePartial(decl, inner);
			else
				CaptureVariableTemplate(decl, inner);
			return;
		}
		if (id->parts.size() > 1)
		{
			// PA21: a prefix resolving to a concrete class (or
			// instantiated specialization) scope defines a member
			// template of that class out of class; a pattern-form
			// prefix registers on the owner template instead.
			if (!TemplateMemberOwnerIsPattern(decl, *id))
			{
				Scope* declaring = ResolvePrefixScope(*id);
				if (declaring->kind == SCOPE_CLASS)
				{
					CaptureQualifiedMemberTemplate(decl, inner,
					                               declaring);
					return;
				}
			}
			RegisterTemplateMember(decl, *id);
			return;
		}
		CaptureFunctionTemplate(decl, inner);
		return;
	}
	case DK_SPECIAL_MEMBER_DEFINITION:
	case DK_SPECIAL_MEMBER_DECLARATION:
	{
		const AstName* id =
			inner.declarator ? inner.declarator->IdName() : 0;
		if (!id || id->parts.size() < 2)
			throw OutsideBoundary("template special-member form");
		if (!TemplateMemberOwnerIsPattern(decl, *id))
		{
			Scope* declaring = ResolvePrefixScope(*id);
			if (declaring->kind == SCOPE_CLASS)
			{
				CaptureQualifiedMemberTemplate(decl, inner, declaring);
				return;
			}
		}
		RegisterTemplateMember(decl, *id);
		return;
	}
	case DK_ALIAS:
		// PA21 alias templates: captured like other templates; a use
		// substitutes into the aliased type-id (14.5.7).
		CaptureAliasTemplate(decl, inner);
		return;
	case DK_TEMPLATE:
	{
		// PA21 `template<A> template<B> ...`: an out-of-class member
		// -template definition. The innermost declarator's qualified
		// name locates the owner class template; the whole outer
		// declaration registers as a member definition and re-binds at
		// each enclosing instantiation.
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
			throw OutsideBoundary("nested template declaration");
		RegisterTemplateMember(decl, *id);
		return;
	}
	default:
		throw OutsideBoundary("templated declaration form");
	}
}

// Whether a qualified member-definition name mentions the
// declaration's own template parameters in its qualifier (the
// pattern form `C<T>::f`, which registers on the owner template
// rather than resolving to a concrete scope).
bool SemBinder::TemplateMemberOwnerIsPattern(const AstDecl& decl,
                                             const AstName& name)
{
	// The declared parameter names (a non-type parameter's name lives
	// in its declarator).
	std::set<string> params;
	vector<TemplateParam> collected;
	CollectTemplateParams(decl, collected);
	for (size_t i = 0; i < collected.size(); i++)
		if (!collected[i].name.empty())
			params.insert(collected[i].name);
	if (params.empty())
		return false;
	for (size_t i = 0; i + 1 < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		for (size_t a = 0; a < part.arguments.size(); a++)
		{
			const AstTemplateArgument& argument = part.arguments[a];
			if (argument.type)
			{
				const AstSpecifierSeq& specs =
					argument.type->specifiers;
				for (size_t k = 0; k < specs.size(); k++)
					if (specs[k].kind == SPEC_TYPE_NAME &&
					    NameMentionsAny(specs[k].name, params))
						return true;
			}
			if (argument.expr &&
			    ExprMentionsAny(*argument.expr, params))
				return true;
		}
	}
	return false;
}

// Whether an expression mentions any of the given parameter names
// (id-expressions, operands, call arguments, and embedded type-ids).
bool SemBinder::ExprMentionsAny(const AstExpr& expr,
                                const std::set<string>& params)
{
	if ((expr.kind == EK_ID || expr.kind == EK_MEMBER) &&
	    NameMentionsAny(expr.name, params))
		return true;
	if (expr.type)
	{
		const AstSpecifierSeq& specs = expr.type->specifiers;
		for (size_t k = 0; k < specs.size(); k++)
			if (specs[k].kind == SPEC_TYPE_NAME &&
			    NameMentionsAny(specs[k].name, params))
				return true;
	}
	for (size_t i = 0; i < expr.operands.size(); i++)
		if (expr.operands[i] && ExprMentionsAny(*expr.operands[i], params))
			return true;
	for (size_t i = 0; i < expr.arguments.size(); i++)
		if (expr.arguments[i] &&
		    ExprMentionsAny(*expr.arguments[i], params))
			return true;
	return false;
}

// True when every non-terminal component of `name` is a plain
// identifier naming a namespace from the current context.
bool SemBinder::QualifierIsNamespacePath(const AstName& name)
{
	Scope* scope = name.global_scope ? model_.global() : 0;
	for (size_t i = 0; i + 1 < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if (part.kind != NP_IDENTIFIER || part.tilde)
			return false;
		const ScopeBinding* found = scope
			? QualifiedLookup(*scope, part.identifier, SLF_SCOPE_NAMES)
			: UnqualifiedLookup(current_, part.identifier,
			                    SLF_SCOPE_NAMES);
		if (!found || (found->kind != SB_NAMESPACE &&
		               found->kind != SB_NAMESPACE_ALIAS))
			return false;
		scope = found->target;
	}
	return true;
}

// The qualified-definition form: the template name binds through the
// terminal component (the prefix scope is already `current_`).
void SemBinder::CaptureQualifiedClassTemplate(const AstDecl& decl,
                                              const AstDecl& inner)
{
	CaptureClassTemplate(decl, inner, true);
}

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
	if (!tmpl.alias_type)
		throw runtime_error("alias template " + tmpl.name +
		                    " has no aliased type");
	string key = TemplateArgumentKey(args);
	unique_ptr<ScopeBinding>& slot = tmpl.dependent_uses[key];
	if (slot)
		return slot.get();
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
	slot.reset(new ScopeBinding());
	slot->kind = SB_TYPE_ALIAS;
	slot->name = tmpl.name;
	slot->type = substituted;
	slot->owner = tmpl.declaring;
	slot->home = tmpl.declaring;
	return slot.get();
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
		// visible upgrade now, then any ready member definitions.
		for (map<string, unique_ptr<ClassSpecialization>>::iterator it =
		         tmpl->class_specs.begin();
		     it != tmpl->class_specs.end(); ++it)
			if (!it->second->instantiated)
				InstantiateClassSpecialization(*tmpl, *it->second);
		InstantiateReadyMembers(*tmpl);
	}
	else if (!tmpl->decl)
	{
		tmpl->decl = &decl;
		tmpl->pattern_decl = &inner;
	}
}

TemplateInfo* SemBinder::CaptureFunctionTemplate(const AstDecl& decl,
                                                 const AstDecl& inner,
                                                 bool as_friend)
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
			if (SameFunctionTemplateSignature(*other, decl, inner))
			{
				merged = other;
				break;
			}
		}
	}
	if (merged)
	{
		if (definition && merged->pattern_decl != &inner)
		{
			if (merged->has_definition)
				throw runtime_error("redefinition of function template " +
				                    name);
			merged->decl = &decl;
			merged->pattern_decl = &inner;
			merged->has_definition = true;
			merged->params = params;
			merged->pattern_ready = false;
			InstantiatePendingFunctions(*merged);
		}
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

// --- template-id resolution ------------------------------------------------

const ScopeBinding* SemBinder::ResolveTemplateIdBinding(
	const AstNamePart& part, Scope* prefix)
{
	const ScopeBinding* found = prefix
		? QualifiedLookup(*prefix, part.identifier, SLF_ANY)
		: UnqualifiedLookup(current_, part.identifier, SLF_ANY);
	if (!found)
		throw runtime_error("undeclared template name " +
		                    part.identifier);
	if (found->kind == SB_FUNCTION)
		return ResolveFunctionTemplateId(*found, part);
	if (found->kind == SB_VARIABLE_TEMPLATE && found->templ)
		return ResolveVariableTemplateId(*found->templ, part);
	TemplateInfo* named_template = found->templ;
	// 14.6.1p1: the injected-class-name of a specialization followed
	// by an argument list acts as the template-name.
	if (!named_template && found->kind == SB_TYPE && found->type &&
	    found->type->kind == TK_CLASS && found->type->named->spec_template)
		named_template = const_cast<TemplateInfo*>(
			found->type->named->spec_template);
	if (!named_template)
		throw runtime_error(part.identifier +
		                    " does not name a template");
	TemplateInfo& tmpl = *named_template;
	vector<TemplateArg> args = ResolveTemplateArgumentList(tmpl, part);
	// PA21 alias templates substitute instead of specializing.
	if (tmpl.kind == TMPL_ALIAS)
		return ResolveAliasTemplateId(tmpl, args);
	// A use through a bound template-template parameter is a pattern
	// use even with concrete arguments: the real template arrives at
	// instantiation.
	bool dependent = tmpl.tt_param_index >= 0;
	for (size_t i = 0; i < args.size(); i++)
		if (TemplateArgIsDependent(args[i]))
			dependent = true;
	if (dependent)
	{
		// A pattern use (`box<T>` with T still abstract): a dependent
		// specialization type, resolved for real at instantiation.
		string key = TemplateArgumentKey(args);
		unique_ptr<ScopeBinding>& slot = tmpl.dependent_uses[key];
		if (!slot)
		{
			slot.reset(new ScopeBinding());
			slot->kind = SB_TYPE;
			slot->name = tmpl.name;
			slot->type = MakeTemplateSpecType(tmpl.anchor, args);
			slot->owner = tmpl.declaring;
			slot->home = tmpl.declaring;
		}
		return slot.get();
	}
	ClassSpecialization* spec = EnsureClassSpecialization(tmpl, args);
	return &spec->self;
}

// --- class instantiation ----------------------------------------------------

Scope* SemBinder::MakeArgumentAliasScope(const TemplateInfo& tmpl,
                                         const vector<TemplateArg>& args)
{
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                  tmpl.declaring);
	// PA21: a deduction-produced slot list (one slot per parameter,
	// pack slots carrying their element runs) binds one-to-one; the
	// span mapping below is for flattened concrete argument lists.
	bool slot_form = false;
	for (size_t i = 0; i < args.size(); i++)
		if (args[i].is_pack_slot)
			slot_form = true;
	if (slot_form && args.size() == tmpl.params.size())
	{
		for (size_t i = 0; i < tmpl.params.size(); i++)
		{
			if (args[i].is_pack_slot)
				BindPackAliasElements(*scope, tmpl.params[i],
				                      args[i].pack_elements);
			else
				BindParamAlias(*scope, tmpl.params[i], args[i]);
		}
		return scope;
	}
	std::vector<std::pair<size_t, size_t>> spans;
	if (!MapParamSpans(tmpl.params, args.size(), spans))
	{
		// A partially-bound (deduction) list without a pack: bind what
		// is known one-to-one.
		for (size_t i = 0; i < args.size() && i < tmpl.params.size();
		     i++)
			BindParamAlias(*scope, tmpl.params[i], args[i]);
		return scope;
	}
	for (size_t i = 0; i < tmpl.params.size(); i++)
	{
		if (tmpl.params[i].pack)
			BindPackAlias(*scope, tmpl.params[i], args,
			              spans[i].first, spans[i].second);
		else if (spans[i].first < spans[i].second)
			BindParamAlias(*scope, tmpl.params[i],
			               args[spans[i].first]);
	}
	return scope;
}

ClassSpecialization* SemBinder::EnsureClassSpecialization(
	TemplateInfo& tmpl, const vector<TemplateArg>& args)
{
	string key = TemplateArgumentKey(args);
	unique_ptr<ClassSpecialization>& slot = tmpl.class_specs[key];
	if (!slot)
	{
		slot.reset(new ClassSpecialization());
		ClassSpecialization* spec = slot.get();
		spec->owner = &tmpl;
		spec->args = args;
		spec->key = key;
		const string spec_name =
			tmpl.name + TemplateArgumentSpelling(args);
		// The display qualifies by the template's declaring scope (the
		// specialization lives there), not the first-use scope.
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			tmpl.anchor->class_key + " " +
				QualifiedScopePath(tmpl.declaring) + spec_name,
			tmpl.declaring, spec_name);
		info->is_union = tmpl.anchor->is_union;
		info->class_key = tmpl.anchor->class_key;
		info->spec_template = &tmpl;
		info->spec_args = args;
		spec->entity = info;
		spec->self.kind = SB_TYPE;
		spec->self.name = tmpl.name;
		spec->self.type = MakeNamedType(TK_CLASS, info);
		spec->self.owner = tmpl.declaring;
		spec->self.home = tmpl.declaring;
	}
	ClassSpecialization* spec = slot.get();
	// 14.7.3: a declared explicit specialization owns its key; uses
	// see an incomplete type until its definition arrives. 14.7.1p4:
	// an argument type still mid-instantiation defers the body to the
	// first completeness demand (EnsureTypeCompleteness).
	if (!spec->instantiated && !spec->explicit_spec &&
	    !SpecializationArgsOpen(*spec))
		InstantiateSpecializationBody(tmpl, *spec);
	return spec;
}

// Whether an argument names a class whose body is still binding (its
// member scope exists but the entity is incomplete).
bool SemBinder::SpecializationArgsOpen(const ClassSpecialization& spec)
{
	for (size_t i = 0; i < spec.args.size(); i++)
	{
		const TemplateArg& arg = spec.args[i];
		if (arg.is_value || !arg.type)
			continue;
		TypePtr bare = RemoveTopCv(arg.type);
		if (bare->kind == TK_CLASS && !bare->named->complete &&
		    model_.MemberScope(bare->named))
			return true;
	}
	return false;
}

// The deferred body path shared by first resolution and the
// completeness demand: a matching partial specialization's pattern
// binds instead of the primary's (14.5.5).
void SemBinder::InstantiateSpecializationBody(TemplateInfo& tmpl,
                                              ClassSpecialization& spec)
{
	vector<TemplateArg> bound;
	int partial = MatchPartialSpecialization(tmpl, spec.args, bound);
	if (partial >= 0)
	{
		InstantiateClassFromPartial(tmpl, spec,
		                            tmpl.partials[partial], bound);
		InstantiateReadyMembers(tmpl);
		InstantiateReadyPartialMembers(tmpl);
	}
	else if (tmpl.has_definition)
	{
		InstantiateClassSpecialization(tmpl, spec);
		InstantiateReadyMembers(tmpl);
	}
}

void SemBinder::InstantiateClassSpecialization(TemplateInfo& tmpl,
                                               ClassSpecialization& spec)
{
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	spec.instantiated = true;
	spec.param_scope = MakeArgumentAliasScope(tmpl, spec.args);
	// The injected pattern name: `Foo` inside the body names this
	// specialization (14.6.1p1), and BindClass completes this entity
	// instead of creating a fresh one.
	ScopeBinding injected;
	injected.kind = SB_TYPE;
	injected.name = tmpl.name;
	injected.type = spec.self.type;
	AddBinding(*spec.param_scope, injected);

	InstantiationContext context(*this, spec.param_scope, true);
	BindDeclaration(*tmpl.pattern_decl);
	// The member scope adopts the specialization spelling so the
	// lowering's scope paths and symbol names see `Foo<int>`.
	if (Scope* members = model_.MemberScope(spec.entity))
		members->name = spec.entity->name;
}

// --- out-of-class member definitions ---------------------------------------

TemplateInfo* SemBinder::ResolveMemberOwnerTemplate(const AstName& id,
                                                    size_t& tmpl_part)
{
	// Walk the leading plain components as namespace qualifiers; the
	// first template-id component names the owner class template.
	Scope* scope = id.global_scope ? model_.global() : 0;
	for (size_t i = 0; i + 1 < id.parts.size(); i++)
	{
		const AstNamePart& part = id.parts[i];
		if (part.kind == NP_TEMPLATE_ID)
		{
			const ScopeBinding* found = scope
				? QualifiedLookup(*scope, part.identifier, SLF_ANY)
				: UnqualifiedLookup(current_, part.identifier, SLF_ANY);
			if (!found || found->kind != SB_CLASS_TEMPLATE)
				return 0;
			tmpl_part = i;
			return found->templ;
		}
		if (part.kind != NP_IDENTIFIER || part.tilde)
			return 0;
		const ScopeBinding* found = scope
			? QualifiedLookup(*scope, part.identifier, SLF_SCOPE_NAMES)
			: UnqualifiedLookup(current_, part.identifier,
			                    SLF_SCOPE_NAMES);
		if (!found)
			return 0;
		if (found->kind != SB_NAMESPACE &&
		    found->kind != SB_NAMESPACE_ALIAS)
			return 0;
		scope = found->target;
	}
	return 0;
}

static string MemberDefName(const AstDecl& decl);

void SemBinder::RegisterTemplateMember(const AstDecl& decl,
                                       const AstName& id)
{
	size_t tmpl_part = 0;
	TemplateInfo* tmpl = ResolveMemberOwnerTemplate(id, tmpl_part);
	if (!tmpl)
		throw OutsideBoundary("qualified template declarator");
	// The member definition's own parameter names rebind positionally
	// at each instantiation; arity must agree with the template.
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	// PA21: a definition whose qualifier pattern matches a partial
	// specialization belongs to that partial (its own parameter list
	// pairs with the partial's).
	if (!tmpl->partials.empty())
	{
		vector<TemplateArg> pattern;
		bool composed = false;
		try
		{
			pattern = ComposePartialPattern(*tmpl, params,
			                                id.parts[tmpl_part]);
			composed = true;
		}
		catch (const std::exception&)
		{
			// Not a partial-form qualifier; the primary check below
			// applies.
		}
		if (composed)
		{
			string key = TemplateArgumentKey(pattern);
			for (size_t p = 0; p < tmpl->partials.size(); p++)
			{
				if (tmpl->partials[p].params.size() != params.size() ||
				    TemplateArgumentKey(tmpl->partials[p].pattern) !=
				        key)
					continue;
				tmpl->partials[p].member_defs.push_back(&decl);
				InstantiateReadyPartialMembers(*tmpl);
				return;
			}
		}
	}
	if (params.size() != tmpl->params.size())
		throw runtime_error("template parameter list of a member of " +
		                    tmpl->name + " disagrees");
	CheckMemberDefinitionAgainstPattern(*tmpl, *decl.inner, params);
	tmpl->member_defs.push_back(&decl);
	InstantiateReadyMembers(*tmpl);
}

// Instantiates the pending member definitions registered on partial
// specializations for every specialization built from them.
void SemBinder::InstantiateReadyPartialMembers(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<ClassSpecialization>>::iterator it =
	         tmpl.class_specs.begin();
	     it != tmpl.class_specs.end(); ++it)
	{
		ClassSpecialization& spec = *it->second;
		if (!spec.instantiated || !spec.entity->complete ||
		    spec.partial_index < 0 ||
		    (size_t)spec.partial_index >= tmpl.partials.size())
			continue;
		PartialSpecialization& partial =
			tmpl.partials[spec.partial_index];
		for (size_t i = 0; i < partial.member_defs.size(); i++)
		{
			if (spec.partial_members_done.count(i))
				continue;
			spec.partial_members_done[i] = true;
			const AstDecl& def = *partial.member_defs[i];
			if (spec.member_spec_names.count(MemberDefName(def)))
				continue;
			if (instantiation_depth_ >=
			    kTemplateInstantiationDepthLimit)
				throw runtime_error("template instantiation depth "
				                    "limit exceeded for " + tmpl.name);
			TemplateInfo shadow;
			vector<TemplateParam> def_params;
			CollectTemplateParams(def, def_params);
			shadow.params = def_params;
			shadow.declaring = tmpl.declaring;
			Scope* alias_scope =
				MakeArgumentAliasScope(shadow, spec.partial_bound);
			InstantiationContext context(*this, alias_scope, true);
			BindDeclaration(*def.inner);
			FlushDeferredBodies();
		}
	}
}

// 15.4p5: an out-of-class special-member definition must repeat the
// declared exception specification; checked syntactically at
// registration (the definition may never instantiate). Pairing goes
// by the canonical parameter spelling (positional parameter identity,
// so same-arity overloads pair correctly); arity is the fallback for
// shapes the canonical spelling cannot express.
void SemBinder::CheckMemberDefinitionAgainstPattern(
	const TemplateInfo& tmpl, const AstDecl& inner,
	const vector<TemplateParam>& def_params)
{
	if (!tmpl.has_definition ||
	    (inner.kind != DK_SPECIAL_MEMBER_DEFINITION &&
	     inner.kind != DK_SPECIAL_MEMBER_DECLARATION))
		return;
	const AstDecl& pattern = *tmpl.pattern_decl;
	const AstName* id = inner.declarator->IdName();
	bool def_dtor = id && !id->parts.empty() && id->parts.back().tilde;
	bool def_noexcept = DeclaratorSpellsNoexcept(*inner.declarator);
	size_t def_arity = DeclaratorArity(*inner.declarator);
	string def_sig = CanonicalDeclaratorParams(*inner.declarator,
	                                           def_params);
	const AstDecl* by_arity = 0;
	for (size_t i = 0; i < pattern.members.size(); i++)
	{
		const AstDecl& member = *pattern.members[i];
		if (member.kind != DK_SPECIAL_MEMBER_DECLARATION ||
		    !member.declarator)
			continue;
		const AstName* member_id = member.declarator->IdName();
		bool member_dtor = member_id && !member_id->parts.empty() &&
			member_id->parts.back().tilde;
		if (member_dtor != def_dtor)
			continue;
		if (DeclaratorArity(*member.declarator) != def_arity)
			continue;
		if (!by_arity)
			by_arity = &member;
		if (CanonicalDeclaratorParams(*member.declarator, tmpl.params) !=
		    def_sig)
			continue;
		if (DeclaratorSpellsNoexcept(*member.declarator) != def_noexcept)
			throw runtime_error("out-of-class definition of a member of "
			                    + tmpl.name +
			                    " changes the exception specification");
		return;
	}
	if (by_arity &&
	    DeclaratorSpellsNoexcept(*by_arity->declarator) != def_noexcept)
		throw runtime_error("out-of-class definition of a member of " +
		                    tmpl.name +
		                    " changes the exception specification");
}

// Whether a registered member definition defines a static data member
// (an object, not a function/class/special member): those instantiate
// on demand (14.7.1p8), not eagerly with the other members.
static bool MemberDefIsStaticData(const AstDecl& decl)
{
	if (!decl.inner || decl.inner->kind != DK_SIMPLE)
		return false;
	const AstDecl& inner = *decl.inner;
	if (inner.declarators.size() != 1 || !inner.declarators[0].declarator)
		return false;
	return !DeclaratorHasParameterClause(
		*inner.declarators[0].declarator);
}

// The terminal declared name of a member definition (empty when it
// has none).
static string MemberDefName(const AstDecl& decl)
{
	const AstDeclarator* declarator = 0;
	if (decl.inner->kind == DK_SIMPLE &&
	    decl.inner->declarators.size() == 1)
		declarator = decl.inner->declarators[0].declarator.get();
	else if (decl.inner->declarator)
		declarator = decl.inner->declarator.get();
	if (!declarator)
		return string();
	const AstName* id = declarator->IdName();
	if (!id || id->parts.empty())
		return string();
	return id->parts.back().identifier;
}

void SemBinder::InstantiateReadyMembers(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<ClassSpecialization>>::iterator it =
	         tmpl.class_specs.begin();
	     it != tmpl.class_specs.end(); ++it)
	{
		ClassSpecialization& spec = *it->second;
		// A specialization whose body is still binding (a member of it
		// re-entered instantiation) picks its members up when its own
		// instantiation completes. The primary's member definitions do
		// not apply to explicit or partial specializations (14.5.5,
		// 14.7.3).
		if (!spec.instantiated || !spec.entity->complete ||
		    spec.explicit_spec || spec.from_partial)
			continue;
		for (size_t i = 0; i < tmpl.member_defs.size(); i++)
		{
			if (spec.members_done.count(i))
				continue;
			// 14.7.3: an explicit member specialization owns its name.
			if (spec.member_spec_names.count(
			        MemberDefName(*tmpl.member_defs[i])))
			{
				spec.members_done[i] = true;
				continue;
			}
			if (MemberDefIsStaticData(*tmpl.member_defs[i]) &&
			    !spec.statics_demanded)
				continue;
			spec.members_done[i] = true;
			InstantiateMemberDefinition(tmpl, spec, i);
		}
	}
}

// Instantiates the pending static-data-member definitions of `spec`
// (all of them, or just those declaring `name`).
void SemBinder::InstantiateStaticMembers(TemplateInfo& tmpl,
                                         ClassSpecialization& spec,
                                         const string* name)
{
	if (!spec.instantiated || !spec.entity->complete ||
	    spec.explicit_spec || spec.from_partial)
		return;
	for (size_t i = 0; i < tmpl.member_defs.size(); i++)
	{
		if (spec.members_done.count(i))
			continue;
		const AstDecl& decl = *tmpl.member_defs[i];
		if (!MemberDefIsStaticData(decl))
			continue;
		if (name && MemberDefName(decl) != *name)
			continue;
		// 14.7.3: an explicit member specialization owns its name.
		spec.members_done[i] = true;
		if (spec.member_spec_names.count(MemberDefName(decl)))
			continue;
		InstantiateMemberDefinition(tmpl, spec, i);
	}
}

// The class-template specialization record owning `entity` (null when
// the entity is not an instantiated specialization).
ClassSpecialization* SemBinder::FindSpecializationRecord(
	const NamedTypeInfo* entity)
{
	if (!entity || !entity->spec_template || entity->is_template_anchor)
		return 0;
	TemplateInfo& tmpl =
		*const_cast<TemplateInfo*>(entity->spec_template);
	map<string, unique_ptr<ClassSpecialization>>::iterator found =
		tmpl.class_specs.find(TemplateArgumentKey(entity->spec_args));
	if (found == tmpl.class_specs.end())
		return 0;
	return found->second.get();
}

// Definition of an object of a specialization type: its static-data
// -member definitions become emittable (the checked references pin
// this demand shape; constant-context reads alone leave no storage).
void SemBinder::DemandSpecializationStatics(const NamedTypeInfo* entity)
{
	ClassSpecialization* spec = FindSpecializationRecord(entity);
	if (!spec || spec->statics_demanded)
		return;
	spec->statics_demanded = true;
	InstantiateStaticMembers(
		*const_cast<TemplateInfo*>(entity->spec_template), *spec, 0);
}

// A non-folding reference to a static data member: instantiate its
// registered out-of-class definition (14.7.1p8 odr-use demand). The
// owning specialization is found from the member's declaring scope
// chain (the member may live in a nested class of the
// specialization).
void SemBinder::OnStaticMemberReferenced(const ScopeBinding& binding)
{
	if (in_unevaluated_operand_)
		return;
	for (const Scope* scope = binding.owner; scope;
	     scope = scope->parent)
	{
		if (scope->kind != SCOPE_CLASS || !scope->entity)
			continue;
		ClassSpecialization* spec =
			FindSpecializationRecord(scope->entity);
		if (!spec)
			continue;
		InstantiateStaticMembers(
			*const_cast<TemplateInfo*>(scope->entity->spec_template),
			*spec, &binding.name);
		return;
	}
}

void SemBinder::InstantiateMemberDefinition(TemplateInfo& tmpl,
                                            ClassSpecialization& spec,
                                            size_t member_index)
{
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	const AstDecl& decl = *tmpl.member_defs[member_index];
	// The definition's own parameter names alias the specialization's
	// arguments; the qualified declarator prefix (`Foo<T>::`) then
	// resolves to this specialization through the ordinary machinery.
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	TemplateInfo shadow;
	shadow.params = params;
	shadow.declaring = tmpl.declaring;
	Scope* alias_scope = MakeArgumentAliasScope(shadow, spec.args);
	InstantiationContext context(*this, alias_scope, true);
	BindDeclaration(*decl.inner);
	FlushDeferredBodies();
}

// --- dependent bases ----------------------------------------------------------

// Whether a base-clause name mentions a template parameter (an alias
// bound in a SCOPE_TEMPLATE_PARAMS scope of the current chain), making
// the base dependent in the pattern (14.6.2p1).
bool SemBinder::BaseClauseIsDependent(const AstName& name)
{
	std::set<string> params;
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (scope->kind == SCOPE_TEMPLATE_PARAMS)
			for (size_t i = 0; i < scope->bindings.size(); i++)
				params.insert(scope->bindings[i].name);
	if (params.empty())
		return false;
	return NameMentionsAny(name, params);
}

bool SemBinder::NameMentionsAny(const AstName& name,
                                const std::set<string>& params)
{
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if (params.count(part.identifier))
			return true;
		for (size_t a = 0; a < part.arguments.size(); a++)
		{
			const AstTemplateArgument& argument = part.arguments[a];
			if (!argument.is_type || !argument.type)
				continue;
			const AstSpecifierSeq& specs = argument.type->specifiers;
			for (size_t k = 0; k < specs.size(); k++)
				if (specs[k].kind == SPEC_TYPE_NAME &&
				    NameMentionsAny(specs[k].name, params))
					return true;
		}
	}
	return false;
}

// --- nested member classes ---------------------------------------------------

void SemBinder::BindClassDeclaration(const AstDecl& decl)
{
	// 14.7.1p1: instantiating a class template instantiates the
	// declarations, not the definitions, of its member classes; the
	// definition completes on first use of the complete type.
	if (instantiating_ && decl.has_name &&
	    decl.class_name.IsPlainIdentifier() &&
	    current_->kind == SCOPE_CLASS)
	{
		TypePtr forward = BindClassForward(decl, false);
		PendingClassDefinition pending;
		pending.decl = &decl;
		pending.scope = current_;
		pending_classes_[forward->named] = pending;
		return;
	}
	// An out-of-class definition of a nested class of a specialization
	// (`struct Foo<int>::Inner { ... };` or the instantiated form of a
	// qualified member-class template definition).
	if (decl.has_name && decl.class_name.parts.size() > 1)
	{
		// A namespace-qualified pattern name inside its own
		// instantiation resolves through the injected alias-scope
		// binding.
		if (instantiating_ &&
		    decl.class_name.parts.back().kind == NP_IDENTIFIER &&
		    FindOwnBinding(*current_,
		                   decl.class_name.parts.back().identifier))
		{
			bool saved_allow = allow_qualified_class_name_;
			allow_qualified_class_name_ = true;
			try
			{
				BindClass(decl, true);
			}
			catch (...)
			{
				allow_qualified_class_name_ = saved_allow;
				throw;
			}
			allow_qualified_class_name_ = saved_allow;
			return;
		}
		Scope* declaring = ResolvePrefixScope(decl.class_name);
		if (declaring->kind != SCOPE_CLASS)
			throw OutsideBoundary("qualified class definition scope");
		Scope* saved = current_;
		bool saved_allow = allow_qualified_class_name_;
		current_ = declaring;
		allow_qualified_class_name_ = true;
		try
		{
			BindClass(decl, true);
		}
		catch (...)
		{
			current_ = saved;
			allow_qualified_class_name_ = saved_allow;
			throw;
		}
		current_ = saved;
		allow_qualified_class_name_ = saved_allow;
		return;
	}
	BindClass(decl, true);
}

void SemBinder::EnsureTypeCompleteness(const NamedTypeInfo* info)
{
	if (!info || info->complete)
		return;
	// PA21 14.7.1p4: a specialization whose body was deferred (an
	// argument was still binding at resolution) instantiates at the
	// first completeness demand.
	if (info->spec_template && !info->is_template_anchor)
	{
		TemplateInfo& tmpl =
			*const_cast<TemplateInfo*>(info->spec_template);
		map<string, unique_ptr<ClassSpecialization>>::iterator spec =
			tmpl.class_specs.find(TemplateArgumentKey(info->spec_args));
		if (spec != tmpl.class_specs.end() &&
		    !spec->second->instantiated &&
		    !spec->second->explicit_spec)
		{
			InstantiateSpecializationBody(tmpl, *spec->second);
			return;
		}
	}
	std::map<const NamedTypeInfo*, PendingClassDefinition>::iterator
		found = pending_classes_.find(info);
	if (found == pending_classes_.end())
		return;
	PendingClassDefinition pending = found->second;
	pending_classes_.erase(found);
	InstantiationContext context(*this, pending.scope, true);
	// The forward-declared entity completes in place (the pending
	// scope is its declaring class scope).
	BindClass(*pending.decl, true);
}

// --- explicit instantiation --------------------------------------------------

void SemBinder::BindExplicitInstantiation(const AstDecl& decl)
{
	if (!decl.inner)
		throw OutsideBoundary("explicit instantiation form");
	const AstDecl& inner = *decl.inner;
	bool is_extern = decl.extern_instantiation;
	if (inner.kind == DK_CLASS_FORWARD)
	{
		const AstName& name = inner.class_name;
		if (name.parts.empty() ||
		    name.parts.back().kind != NP_TEMPLATE_ID)
			throw OutsideBoundary("explicit instantiation of a "
			                      "non-template-id");
		// Resolving the template-id instantiates the class; the eager
		// member-definition instantiation covers 14.7.2p8 for the
		// supported subset.
		const ScopeBinding* binding = ResolveTerminal(name, SLF_ANY);
		if (!binding || binding->kind != SB_TYPE ||
		    binding->type->kind != TK_CLASS)
			throw runtime_error("explicit instantiation does not name "
			                    "a class specialization");
		ClassSpecialization* spec =
			FindSpecializationRecord(binding->type->named);
		if (is_extern)
		{
			// An explicit-instantiation declaration suppresses local
			// emission; a later definition emits on ordinary demand
			// (the checked references pin this combination).
			if (spec)
				spec->extern_declared = true;
			return;
		}
		if (!binding->type->named->complete)
			throw runtime_error("explicit instantiation of an "
			                    "undefined class template");
		if (!spec || !spec->extern_declared)
			unit_.explicit_instantiations.push_back(binding->type->named);
		return;
	}
	if (inner.kind == DK_SIMPLE)
	{
		BindExplicitFunctionInstantiation(inner, is_extern);
		return;
	}
	if (inner.kind == DK_SPECIAL_MEMBER_DECLARATION ||
	    inner.kind == DK_SPECIAL_MEMBER_DEFINITION)
	{
		// `extern template box<int>::box();`: naming the constructor
		// instantiates the class; the demand-driven member emission
		// already defers what the declaration form suppresses.
		const AstName* id =
			inner.declarator ? inner.declarator->IdName() : 0;
		if (!id || id->parts.size() < 2)
			throw OutsideBoundary("explicit instantiation form");
		Scope* declaring = ResolvePrefixScope(*id);
		if (!declaring || declaring->kind != SCOPE_CLASS)
			throw runtime_error("explicit instantiation of a "
			                    "non-member special member");
		return;
	}
	throw OutsideBoundary("explicit instantiation form");
}
