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

void SemBinder::CollectTemplateParams(const AstDecl& decl,
                                      vector<TemplateParam>& params)
{
	for (size_t i = 0; i < decl.template_params.size(); i++)
	{
		const AstTemplateParameter& parameter = decl.template_params[i];
		if (parameter.kind != TP_TYPE)
			throw OutsideBoundary("non-type or template template "
			                      "parameter");
		if (parameter.pack)
			throw OutsideBoundary("template parameter pack");
		TemplateParam param;
		param.name = parameter.name;
		if (parameter.has_default_type)
			param.default_type = parameter.default_type.get();
		params.push_back(param);
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
	if (!decl.has_parameter_list || !decl.inner)
		throw OutsideBoundary("explicit specialization");
	const AstDecl& inner = *decl.inner;
	if (current_->kind == SCOPE_CLASS)
		throw OutsideBoundary("member template");
	if (current_->kind != SCOPE_NAMESPACE)
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
			RegisterTemplateMember(decl, inner.class_name);
			return;
		}
		throw OutsideBoundary("class template partial specialization");
	case DK_CLASS_FORWARD:
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
		// PA18: variable templates (and their partial specializations)
		// are outside the semantic scope; an uninstantiated
		// declaration parses and is otherwise ignored. A qualified
		// declarator is a class-template member definition instead.
		if (inner.kind == DK_SIMPLE && id->parts.size() == 1 &&
		    !DeclaratorHasParameterClause(
		        *inner.declarators[0].declarator))
			return;
		if (id->parts.size() > 1)
		{
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
		RegisterTemplateMember(decl, *id);
		return;
	}
	case DK_ALIAS:
		throw OutsideBoundary("alias template");
	case DK_TEMPLATE:
		throw OutsideBoundary("nested template declaration");
	default:
		throw OutsideBoundary("templated declaration form");
	}
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

void SemBinder::CaptureClassTemplate(const AstDecl& decl,
                                     const AstDecl& inner, bool definition)
{
	const string& name = inner.class_name.parts.back().identifier;
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
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
			if (params[i].default_type && tmpl->params[i].default_type)
				throw runtime_error("template parameter redefines a "
				                    "default argument");
			if (!params[i].default_type)
				params[i].default_type = tmpl->params[i].default_type;
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

void SemBinder::CaptureFunctionTemplate(const AstDecl& decl,
                                        const AstDecl& inner)
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
		if (definition)
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
		return;
	}
	TemplateInfo* tmpl = unit_.templates.Create(TMPL_FUNCTION, name,
	                                            current_);
	tmpl->params = params;
	tmpl->decl = &decl;
	tmpl->pattern_decl = &inner;
	tmpl->has_definition = definition;
	if (definition)
		CheckTemplateDefinitionSanity(*tmpl);
	if (!binding)
	{
		ScopeBinding fresh;
		fresh.kind = SB_FUNCTION;
		fresh.name = name;
		fresh.access = current_access_;
		binding = &AddBinding(*current_, fresh);
	}
	binding->fn_templates.push_back(tmpl);
}

// --- template-id resolution ------------------------------------------------

vector<TemplateArg> SemBinder::ResolveTemplateArgumentList(
	TemplateInfo& tmpl, const AstNamePart& part)
{
	vector<TemplateArg> args;
	for (size_t i = 0; i < part.arguments.size(); i++)
	{
		const AstTemplateArgument& argument = part.arguments[i];
		if (argument.pack)
			throw OutsideBoundary("pack-expansion template argument");
		if (!argument.is_type || !argument.type)
			throw OutsideBoundary("non-type template argument");
		args.push_back(
			TemplateArg(builder_.ResolveTypeId(*argument.type)));
	}
	if (args.size() > tmpl.params.size())
		throw runtime_error("too many template arguments for " +
		                    tmpl.name);
	if (args.size() < tmpl.params.size())
	{
		// Defaults resolve in the template's declaring scope with the
		// earlier parameters bound to the resolved arguments (14.1).
		Scope* partial = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
		                                    tmpl.declaring);
		for (size_t i = 0; i < args.size(); i++)
			BindParamAlias(*partial, tmpl.params[i], args[i]);
		for (size_t k = args.size(); k < tmpl.params.size(); k++)
		{
			if (!tmpl.params[k].default_type)
				throw runtime_error("too few template arguments for " +
				                    tmpl.name);
			Scope* saved = current_;
			current_ = partial;
			TypePtr resolved;
			try
			{
				resolved = builder_.ResolveTypeId(
					*tmpl.params[k].default_type);
			}
			catch (...)
			{
				current_ = saved;
				throw;
			}
			current_ = saved;
			args.push_back(TemplateArg(resolved));
			BindParamAlias(*partial, tmpl.params[k], args.back());
		}
	}
	return args;
}

// Binds one parameter name to its argument in an alias scope: type
// arguments as type aliases, value arguments as objectless constant
// variables (reads fold; LookupConstant sees the value).
void SemBinder::BindParamAlias(Scope& scope, const TemplateParam& param,
                               const TemplateArg& arg)
{
	if (param.name.empty())
		return;
	ScopeBinding alias;
	if (!arg.is_value)
	{
		alias.kind = SB_TYPE_ALIAS;
		alias.name = param.name;
		alias.type = arg.type;
	}
	else
	{
		alias.kind = SB_VARIABLE;
		alias.name = param.name;
		alias.type = MakeCvQualifiedType(arg.type, true, false);
		alias.has_value = true;
		alias.value = ConstValue(arg.value_type, arg.value_bits);
		alias.no_object = true;
	}
	AddBinding(scope, alias);
}

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
	bool dependent = false;
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
	for (size_t i = 0; i < args.size() && i < tmpl.params.size(); i++)
		BindParamAlias(*scope, tmpl.params[i], args[i]);
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
	if (!spec->instantiated && tmpl.has_definition)
	{
		InstantiateClassSpecialization(tmpl, *spec);
		InstantiateReadyMembers(tmpl);
	}
	return spec;
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
	if (params.size() != tmpl->params.size())
		throw runtime_error("template parameter list of a member of " +
		                    tmpl->name + " disagrees");
	CheckMemberDefinitionAgainstPattern(*tmpl, *decl.inner, params);
	tmpl->member_defs.push_back(&decl);
	InstantiateReadyMembers(*tmpl);
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

void SemBinder::InstantiateReadyMembers(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<ClassSpecialization>>::iterator it =
	         tmpl.class_specs.begin();
	     it != tmpl.class_specs.end(); ++it)
	{
		ClassSpecialization& spec = *it->second;
		// A specialization whose body is still binding (a member of it
		// re-entered instantiation) picks its members up when its own
		// instantiation completes.
		if (!spec.instantiated || !spec.entity->complete)
			continue;
		for (size_t i = 0; i < tmpl.member_defs.size(); i++)
		{
			if (spec.members_done.count(i))
				continue;
			spec.members_done[i] = true;
			InstantiateMemberDefinition(tmpl, spec, i);
		}
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
		if (!binding->type->named->complete)
			throw runtime_error("explicit instantiation of an "
			                    "undefined class template");
		unit_.explicit_instantiations.push_back(binding->type->named);
		return;
	}
	if (inner.kind == DK_SIMPLE)
	{
		BindExplicitFunctionInstantiation(inner);
		return;
	}
	throw OutsideBoundary("explicit instantiation form");
}
