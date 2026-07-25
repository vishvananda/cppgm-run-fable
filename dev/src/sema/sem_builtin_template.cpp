#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::unique_ptr;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA34 assignment boundary");
}

}  // namespace

// PA34 builtin templates referenced by name from hosted sources:
// __type_pack_element<I, Ts...> (a Clang builtin alias template
// selecting the I-th element of its type list) and
// __is_nothrow_invocable<F, Args...> (the GCC-13 builtin trait
// template the checked-in references pin). Each rides a shadow
// TemplateInfo so ordinary argument resolution, pack expansion, and
// the deferred-alias machinery treat it like any alias template;
// ResolveAliasTemplateId (sem_spec.cpp) routes dependent re-uses back
// here.

TemplateInfo& SemBinder::TypePackElementTemplate()
{
	if (!type_pack_element_tmpl_)
	{
		type_pack_index_param_.reset(new AstTemplateParameter());
		AstTemplateParameter& index = *type_pack_index_param_;
		index.kind = TP_NON_TYPE;
		AstSpecifier spec_unsigned;
		spec_unsigned.kind = SPEC_KEYWORD;
		spec_unsigned.keyword = KW_UNSIGNED;
		spec_unsigned.spelling = "unsigned";
		index.specifiers.push_back(std::move(spec_unsigned));
		AstSpecifier spec_long;
		spec_long.kind = SPEC_KEYWORD;
		spec_long.keyword = KW_LONG;
		spec_long.spelling = "long";
		index.specifiers.push_back(std::move(spec_long));
		type_pack_element_tmpl_.reset(new TemplateInfo());
		TemplateInfo& tmpl = *type_pack_element_tmpl_;
		tmpl.name = "__type_pack_element";
		tmpl.kind = TMPL_ALIAS;
		tmpl.declaring = model_.global();
		TemplateParam index_param;
		index_param.kind = TPK_VALUE;
		index_param.name = "__i";
		index_param.source = &index;
		tmpl.params.push_back(index_param);
		TemplateParam pack_param;
		pack_param.kind = TPK_TYPE;
		pack_param.pack = true;
		pack_param.name = "__ts";
		tmpl.params.push_back(pack_param);
		tmpl.anchor = model_.CreateNamedTypeInfo(
			"__type_pack_element", model_.global(),
			"__type_pack_element");
		tmpl.anchor->is_template_anchor = true;
		tmpl.anchor->spec_template = &tmpl;
	}
	return *type_pack_element_tmpl_;
}

const ScopeBinding* SemBinder::ResolveTypePackElementUse(
	const vector<TemplateArg>& args)
{
	TemplateInfo& tmpl = TypePackElementTemplate();
	string key = TemplateArgumentKey(args);
	unique_ptr<ScopeBinding>& slot = tmpl.dependent_uses[key];
	if (slot)
		return slot.get();
	slot.reset(new ScopeBinding());
	slot->kind = SB_TYPE_ALIAS;
	slot->name = tmpl.name;
	slot->owner = tmpl.declaring;
	slot->home = tmpl.declaring;
	bool dependent = false;
	for (size_t i = 0; i < args.size(); i++)
		if (TemplateArgIsDependent(args[i]))
			dependent = true;
	if (dependent)
	{
		// Deferred like a dependent alias use; instantiation
		// re-resolves through ResolveAliasTemplateId.
		slot->type = MakeTemplateSpecType(tmpl.anchor, args);
		return slot.get();
	}
	if (args.empty() || !args[0].is_value)
		throw runtime_error("__type_pack_element index is not a "
		                    "constant");
	size_t index = (size_t)args[0].value_bits;
	if (index + 1 >= args.size() || args[index + 1].is_value ||
	    !args[index + 1].type)
		throw runtime_error("__type_pack_element index is out of "
		                    "range");
	slot->type = args[index + 1].type;
	return slot.get();
}

// The builtin trait template wins over a user class template of the
// same name (the GCC-13 behavior the references pin); the use
// resolves to a synthesized record whose `value` member carries the
// probe result.
TemplateInfo& SemBinder::NothrowInvocableTemplate()
{
	if (!nothrow_invocable_tmpl_)
	{
		nothrow_invocable_tmpl_.reset(new TemplateInfo());
		TemplateInfo& tmpl = *nothrow_invocable_tmpl_;
		tmpl.name = "__is_nothrow_invocable";
		tmpl.kind = TMPL_ALIAS;
		tmpl.declaring = model_.global();
		TemplateParam fn_param;
		fn_param.kind = TPK_TYPE;
		fn_param.name = "__f";
		tmpl.params.push_back(fn_param);
		TemplateParam pack_param;
		pack_param.kind = TPK_TYPE;
		pack_param.pack = true;
		pack_param.name = "__args";
		tmpl.params.push_back(pack_param);
		tmpl.anchor = model_.CreateNamedTypeInfo(
			"__is_nothrow_invocable", model_.global(),
			"__is_nothrow_invocable");
		tmpl.anchor->is_template_anchor = true;
		tmpl.anchor->spec_template = &tmpl;
	}
	return *nothrow_invocable_tmpl_;
}

const ScopeBinding* SemBinder::ResolveNothrowInvocableUse(
	const vector<TemplateArg>& args)
{
	TemplateInfo& tmpl = NothrowInvocableTemplate();
	string key = TemplateArgumentKey(args);
	unique_ptr<ScopeBinding>& slot = tmpl.dependent_uses[key];
	if (slot)
		return slot.get();
	slot.reset(new ScopeBinding());
	slot->kind = SB_TYPE_ALIAS;
	slot->name = tmpl.name;
	slot->owner = tmpl.declaring;
	slot->home = tmpl.declaring;
	bool dependent = false;
	for (size_t i = 0; i < args.size(); i++)
		if (TemplateArgIsDependent(args[i]))
			dependent = true;
	if (dependent)
	{
		slot->type = MakeTemplateSpecType(tmpl.anchor, args);
		return slot.get();
	}
	vector<TypePtr> types;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (args[i].is_value || !args[i].type)
			throw runtime_error("__is_nothrow_invocable takes type "
			                    "arguments");
		types.push_back(args[i].type);
	}
	bool no_throw = false;
	bool can = analyzer_.ProbeTraitInvocable(types, no_throw);
	bool value = can && no_throw;
	const string spec_name =
		tmpl.name + TemplateArgumentSpelling(args);
	NamedTypeInfo* info = model_.CreateNamedTypeInfo(
		"struct " + spec_name, model_.global(), spec_name);
	info->class_key = "struct";
	info->complete = true;
	info->size = 1;
	info->alignment = 1;
	Scope* members = model_.CreateScope(SCOPE_CLASS, spec_name,
	                                    model_.global());
	model_.SetMemberScope(info, members);
	ScopeBinding value_binding;
	value_binding.kind = SB_VARIABLE;
	value_binding.name = "value";
	value_binding.no_object = true;
	value_binding.has_value = true;
	value_binding.value = ConstValue(FT_BOOL, value ? 1 : 0);
	value_binding.type = MakeCvQualifiedType(
		MakeFundamentalType(FT_BOOL), true, false);
	AddBinding(*members, value_binding);
	slot->type = MakeNamedType(TK_CLASS, info);
	return slot.get();
}

// PA34 GNU complex: `_Complex T` models as a synthesized two-field
// record ({__real, __imag}) bound at global scope on first use, so
// layout, copies, and the host ABI classification follow the ordinary
// class rules (the SysV complex ABI matches the pair layout). This is
// the compile-acceptance model; complex arithmetic stays a boundary.
TypePtr SemBinder::MakeGnuComplexType(const TypePtr& element)
{
	TypePtr bare = RemoveTopCv(element);
	if (bare->kind != TK_FUNDAMENTAL)
		throw OutsideBoundary("complex element type");
	EFundamentalType ft = bare->fundamental;
	if (ft != FT_FLOAT && ft != FT_DOUBLE && ft != FT_LONG_DOUBLE)
		throw OutsideBoundary("complex element type");
	std::map<int, TypePtr>::iterator found = complex_types_.find(ft);
	if (found != complex_types_.end())
		return found->second;
	const char* stem = ft == FT_FLOAT ? "__cppgm_complex_float"
		: ft == FT_DOUBLE ? "__cppgm_complex_double"
		                  : "__cppgm_complex_long_double";
	AstDeclPtr decl(new AstDecl(DK_CLASS));
	decl->has_name = true;
	AstNamePart name_part;
	name_part.kind = NP_IDENTIFIER;
	name_part.identifier = stem;
	decl->class_name.parts.push_back(std::move(name_part));
	decl->class_key = KW_STRUCT;
	decl->class_key_spelling = "struct";
	const char* field_names[2] = {"__real", "__imag"};
	for (int f = 0; f < 2; f++)
	{
		AstDeclPtr member(new AstDecl(DK_SIMPLE));
		if (ft == FT_LONG_DOUBLE)
		{
			AstSpecifier spec_long;
			spec_long.kind = SPEC_KEYWORD;
			spec_long.keyword = KW_LONG;
			spec_long.spelling = "long";
			member->specifiers.push_back(std::move(spec_long));
		}
		AstSpecifier spec_base;
		spec_base.kind = SPEC_KEYWORD;
		spec_base.keyword = ft == FT_FLOAT ? KW_FLOAT : KW_DOUBLE;
		spec_base.spelling = ft == FT_FLOAT ? "float" : "double";
		member->specifiers.push_back(std::move(spec_base));
		AstInitDeclarator declarator;
		declarator.declarator.reset(new AstDeclarator());
		AstDeclaratorItem id_item;
		id_item.kind = DI_ID;
		AstNamePart id_part;
		id_part.kind = NP_IDENTIFIER;
		id_part.identifier = field_names[f];
		id_item.name.parts.push_back(std::move(id_part));
		declarator.declarator->items.push_back(std::move(id_item));
		member->declarators.push_back(std::move(declarator));
		decl->members.push_back(std::move(member));
	}
	// Bind at global scope, detached from the open sem-tree position.
	Scope* saved = current_;
	current_ = model_.global();
	SemNodePtr holder = MakeSemNode(SN_COMPOUND_STATEMENT);
	parents_.push_back(holder.get());
	TypePtr type;
	try
	{
		BindClassDeclaration(*decl);
		type = ResolveTypeName(decl->class_name);
	}
	catch (...)
	{
		parents_.pop_back();
		current_ = saved;
		throw;
	}
	parents_.pop_back();
	current_ = saved;
	synth_decls_.push_back(std::move(decl));
	complex_types_[ft] = type;
	return type;
}

// PA25 18.9: the std::initializer_list<element> specialization type;
// throws when the program declares no such template.
TypePtr SemBinder::StdInitializerListType(const TypePtr& element)
{
	Scope* global = model_.global();
	ScopeBinding* std_binding =
		global ? FindOwnBinding(*global, "std") : 0;
	TemplateInfo* tmpl = 0;
	if (std_binding && std_binding->kind == SB_NAMESPACE &&
	    std_binding->target)
		if (ScopeBinding* binding = FindOwnBinding(*std_binding->target,
		                                           "initializer_list"))
			tmpl = binding->templ;
	if (!tmpl || tmpl->kind != TMPL_CLASS)
		throw runtime_error(
			"braced deduction requires std::initializer_list");
	std::vector<TemplateArg> args;
	args.push_back(TemplateArg(element));
	ClassSpecialization* spec = EnsureClassSpecialization(*tmpl, args);
	if (!spec || !spec->entity)
		throw runtime_error(
			"std::initializer_list specialization failed");
	return MakeNamedType(TK_CLASS, spec->entity);
}

// PA25 18.9: the builtin std::initializer_list<T> record for a
// program that only declares the template: {const T* __begin_,
// long __size_}, 16 bytes.
void SemBinder::BuildBuiltinInitializerList(NamedTypeInfo* info)
{
	if (info->complete)
		return;
	TypePtr element = RemoveTopCv(info->spec_args[0].type);
	ClassInfo& cls = unit_.classes.Create(info);
	Scope* members = model_.MemberScope(info);
	if (!members)
	{
		members = model_.CreateScope(
			SCOPE_CLASS, info->name,
			const_cast<Scope*>(
				info->spec_template->declaring
					? info->spec_template->declaring
					: model_.global()));
		model_.SetMemberScope(info, members);
	}
	cls.members = members;
	cls.is_aggregate = false;
	model_.MutableInfo(info)->class_record = &cls;
	BeginClassLayout(cls);
	static const char* const names[2] = {"__begin_", "__size_"};
	TypePtr types[2];
	types[0] = MakePointerType(
		MakeCvQualifiedType(element, true, false), false, false);
	types[1] = MakeFundamentalType(FT_LONG_INT);
	for (int i = 0; i < 2; i++)
	{
		ScopeBinding binding;
		binding.kind = SB_VARIABLE;
		binding.name = names[i];
		binding.type = types[i];
		binding.home = members;
		AddBinding(*members, binding);
		ClassField field;
		field.name = names[i];
		field.type = types[i];
		field.access = MA_PUBLIC;
		LayoutField(cls, field);
	}
	FinishClassLayout(cls, *model_.MutableInfo(info), 0);
	model_.MutableInfo(info)->complete = true;
	// The record copies and destroys like any trivial class (list
	// values pass by value, 18.9p2).
	DeclareImplicitSpecialMembers(cls);
}
