#include "sema/decl_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::to_string;
using std::vector;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA11 assignment boundary");
}

unsigned long long RoundUp(unsigned long long value,
                           unsigned long long alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

// True for types whose layout depends on a template parameter; such a
// class completes without layout facts instead of failing.
bool LayoutDependent(const TypePtr& type)
{
	switch (type->kind)
	{
	case TK_TYPE_PARAM:
		return true;
	case TK_CLASS:
	case TK_ENUM:
		return type->named->complete && type->named->alignment == 0;
	case TK_ARRAY:
		return LayoutDependent(type->target);
	default:
		return false;
	}
}

}  // namespace

DeclBinder::DeclBinder(TypesModel& model)
	: model_(model), builder_(*this), current_(model.global()),
	  current_fields_(0), anonymous_enums_(0)
{
}

void DeclBinder::BindTranslationUnit(const AstDecl& unit)
{
	current_ = model_.global();
	BindDeclarations(unit.body_decls);
}

// --- names -------------------------------------------------------------

const string& DeclBinder::PartName(const AstNamePart& part)
{
	if (part.kind != NP_IDENTIFIER || part.tilde)
		throw OutsideBoundary("name form");
	return part.identifier;
}

const string& DeclBinder::TerminalName(const AstName& name)
{
	if (name.parts.empty())
		throw OutsideBoundary("name form");
	return PartName(name.parts.back());
}

Scope* DeclBinder::ScopeOfBinding(const ScopeBinding& binding)
{
	switch (binding.kind)
	{
	case SB_NAMESPACE:
	case SB_NAMESPACE_ALIAS:
		return binding.target;
	case SB_TYPE:
	case SB_TYPE_ALIAS:
	{
		const TypePtr& type = binding.type;
		if (type->kind != TK_CLASS && type->kind != TK_ENUM)
			break;
		Scope* members = model_.MemberScope(type->named);
		if (!members)
			throw runtime_error(type->named->display +
			                    " has no members to look into");
		return members;
	}
	default:
		break;
	}
	throw runtime_error(binding.name + " does not name a scope");
}

Scope* DeclBinder::ResolvePrefixScope(const AstName& name)
{
	Scope* scope;
	size_t index = 0;
	if (name.global_scope)
		scope = model_.global();
	else
	{
		const ScopeBinding* root = UnqualifiedLookup(
			current_, PartName(name.parts[0]), SLF_SCOPE_NAMES);
		if (!root)
			throw runtime_error("undeclared name " +
			                    name.parts[0].identifier);
		scope = ScopeOfBinding(*root);
		index = 1;
	}
	for (; index + 1 < name.parts.size(); index++)
	{
		const string& part = PartName(name.parts[index]);
		const ScopeBinding* found =
			QualifiedLookup(*scope, part, SLF_SCOPE_NAMES);
		if (!found)
			throw runtime_error("no member named " + part);
		scope = ScopeOfBinding(*found);
	}
	return scope;
}

const ScopeBinding* DeclBinder::ResolveTerminal(const AstName& name,
                                                EScopeLookupFilter filter)
{
	const string& terminal = TerminalName(name);
	if (name.parts.size() == 1 && !name.global_scope)
		return UnqualifiedLookup(current_, terminal, filter);
	return QualifiedLookup(*ResolvePrefixScope(name), terminal, filter);
}

Scope* DeclBinder::ResolveNamespaceTarget(const AstName& name)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_SCOPE_NAMES);
	if (!found ||
	    (found->kind != SB_NAMESPACE && found->kind != SB_NAMESPACE_ALIAS))
		throw runtime_error("expected a namespace name");
	return found->target;
}

// --- interface implementations -------------------------------------------

TypePtr DeclBinder::BindNestedTypeSpecifier(const AstDecl& decl)
{
	switch (decl.kind)
	{
	case DK_CLASS:
		return BindClass(decl, false);
	case DK_CLASS_FORWARD:
		return BindClassForward(decl, true);
	case DK_ENUM:
		return BindEnum(decl);
	default:
		throw OutsideBoundary("declaration used as a type specifier");
	}
}

TypePtr DeclBinder::ResolveTypeName(const AstName& name)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	if (!found)
		throw runtime_error("undeclared type name " + TerminalName(name));
	if (found->kind != SB_TYPE && found->kind != SB_TYPE_ALIAS)
		throw runtime_error(found->name + " does not name a type");
	return found->type;
}

// 7.1.6.2p4 subset: the declared type of a (possibly parenthesized)
// id-expression; the parenthesized form of an lvalue entity yields an
// lvalue reference.
TypePtr DeclBinder::ResolveDecltype(const AstExpr& expr)
{
	const AstExpr* inner = &expr;
	bool parenthesized = false;
	while (inner->kind == EK_PAREN)
	{
		parenthesized = true;
		inner = inner->operands[0].get();
	}
	if (inner->kind != EK_ID)
		throw OutsideBoundary("decltype operand form");
	bool lvalue_entity = false;
	TypePtr declared = DeclaredEntityType(inner->name, lvalue_entity);
	if (parenthesized && lvalue_entity)
		return MakeReferenceType(declared, false, true);
	return declared;
}

TypePtr DeclBinder::DeclaredEntityType(const AstName& name,
                                       bool& lvalue_entity)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	if (!found)
		throw runtime_error("undeclared name " + TerminalName(name));
	switch (found->kind)
	{
	case SB_VARIABLE:
	case SB_PARAMETER:
	case SB_FUNCTION:
		lvalue_entity = true;
		return found->type;
	case SB_ENUMERATOR:
		lvalue_entity = false;
		return found->type;
	default:
		throw OutsideBoundary("decltype of a non-entity name");
	}
}

unsigned long long DeclBinder::EvaluateArrayBound(const AstExpr& expr)
{
	ConstValue value = EvaluateConstExpr(expr, *this);
	if (IsSignedIntegralFundamental(value.type) &&
	    (long long)value.bits < 0)
		throw runtime_error("negative array bound");
	if (value.bits == 0)
		throw runtime_error("zero-size arrays are ill-formed");
	return value.bits;
}

ConstValue DeclBinder::LookupConstant(const AstName& name)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	if (!found)
		throw runtime_error("undeclared name " + TerminalName(name));
	if (!found->has_value)
		throw runtime_error(found->name +
		                    " is not a constant of the PA11 subset");
	return found->value;
}

TypePtr DeclBinder::TryResolveTypeFromName(const AstName& name)
{
	try
	{
		return ResolveTypeName(name);
	}
	catch (const std::exception&)
	{
		// The name does not (unambiguously) name a type; the caller's
		// expression form is then outside the subset and throws there.
		return TypePtr();
	}
}

TypePtr DeclBinder::ResolveTypeId(const AstTypeId& type_id)
{
	return builder_.ResolveTypeId(type_id);
}

// --- declarations --------------------------------------------------------

void DeclBinder::BindDeclarations(const vector<AstDeclPtr>& decls)
{
	for (size_t i = 0; i < decls.size(); i++)
		BindDeclaration(*decls[i]);
}

void DeclBinder::BindDeclaration(const AstDecl& decl)
{
	switch (decl.kind)
	{
	case DK_EMPTY:
	case DK_ACCESS_LABEL:
	// Special members do not enter the PA11 declaration model.
	case DK_SPECIAL_MEMBER_DECLARATION:
	case DK_SPECIAL_MEMBER_DEFINITION:
		return;
	case DK_TRANSLATION_UNIT:
	case DK_LINKAGE:
		BindDeclarations(decl.body_decls);
		return;
	case DK_NAMESPACE:
		BindNamespace(decl);
		return;
	case DK_NAMESPACE_ALIAS:
		BindNamespaceAlias(decl);
		return;
	case DK_USING_DIRECTIVE:
		BindUsingDirective(decl);
		return;
	case DK_USING_DECLARATION:
		BindUsingDeclaration(decl);
		return;
	case DK_ALIAS:
		BindTypeAlias(decl.name, builder_.ResolveTypeId(*decl.type));
		return;
	case DK_TEMPLATE:
		BindTemplateDeclaration(decl);
		return;
	case DK_CLASS:
		BindClass(decl, true);
		return;
	case DK_CLASS_FORWARD:
		BindClassForward(decl, false);
		return;
	case DK_ENUM:
		BindEnum(decl);
		return;
	case DK_STATIC_ASSERT:
		BindStaticAssert(decl);
		return;
	case DK_SIMPLE:
		BindSimpleDeclaration(decl);
		return;
	case DK_FUNCTION:
		BindFunctionDefinition(decl);
		return;
	case DK_BIT_FIELD:
		BindBitFieldDeclaration(decl);
		return;
	case DK_EXPLICIT_INSTANTIATION:
		break;
	}
	throw OutsideBoundary("declaration form");
}

void DeclBinder::BindNamespace(const AstDecl& decl)
{
	if (current_->kind != SCOPE_NAMESPACE)
		throw runtime_error("namespace definition outside a namespace");
	Scope* scope;
	if (decl.unnamed)
	{
		// 7.3.1.1p1: one unnamed member per scope, with the implicit
		// using-directive.
		if (!current_->unnamed_member)
		{
			current_->unnamed_member =
				model_.CreateScope(SCOPE_NAMESPACE, "", current_);
			AddUsingDirective(*current_, current_->unnamed_member);
		}
		scope = current_->unnamed_member;
	}
	else if (ScopeBinding* existing = FindOwnBinding(*current_, decl.name))
	{
		// 7.3.1.2: extension requires the original namespace-name (an
		// alias or any other kind of name conflicts).
		if (existing->kind != SB_NAMESPACE)
			throw runtime_error(decl.name + " is not a namespace");
		scope = existing->target;
	}
	else
	{
		scope = model_.CreateScope(SCOPE_NAMESPACE, decl.name, current_);
		ScopeBinding binding;
		binding.kind = SB_NAMESPACE;
		binding.name = decl.name;
		binding.target = scope;
		AddBinding(*current_, binding);
	}
	// 7.3.1p9: an inline namespace acts through an implicit directive
	// in its parent.
	if (decl.inline_namespace)
		AddUsingDirective(*current_, scope);
	Scope* saved = current_;
	current_ = scope;
	BindDeclarations(decl.body_decls);
	current_ = saved;
}

void DeclBinder::BindNamespaceAlias(const AstDecl& decl)
{
	Scope* target = ResolveNamespaceTarget(decl.target);
	if (ScopeBinding* existing = FindOwnBinding(*current_, decl.name))
	{
		// 7.3.2p3: redefinition is allowed when it does not change the
		// meaning.
		if (existing->kind != SB_NAMESPACE_ALIAS ||
		    existing->target != target)
			throw runtime_error("conflicting namespace alias " + decl.name);
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_NAMESPACE_ALIAS;
	binding.name = decl.name;
	binding.target = target;
	AddBinding(*current_, binding);
}

void DeclBinder::BindUsingDirective(const AstDecl& decl)
{
	AddUsingDirective(*current_, ResolveNamespaceTarget(decl.target));
}

void DeclBinder::BindUsingDeclaration(const AstDecl& decl)
{
	const AstName& target = decl.target;
	if (target.parts.empty() ||
	    (target.parts.size() < 2 && !target.global_scope))
		throw runtime_error("using-declaration requires a qualified name");
	// TerminalName rejects template-ids, operators, and destructor
	// names (7.3.3p5 and the PA11 boundary).
	const string& name = TerminalName(target);
	const ScopeBinding* found =
		QualifiedLookup(*ResolvePrefixScope(target), name, SLF_ANY);
	if (!found)
		throw runtime_error("using-declaration target not found: " + name);
	if (found->kind == SB_NAMESPACE || found->kind == SB_NAMESPACE_ALIAS)
		throw runtime_error("using-declaration shall not name a namespace");
	// The imported binding shares the entity's type, value, and member
	// scope, and prints in the current scope under its own kind.
	ScopeBinding imported = *found;
	AddBinding(*current_, imported);
}

void DeclBinder::BindStaticAssert(const AstDecl& decl)
{
	if (!ConstValueIsNonZero(EvaluateConstExpr(*decl.assert_expr, *this)))
		throw runtime_error("static_assert failed " + decl.message);
}

void DeclBinder::BindSimpleDeclaration(const AstDecl& decl)
{
	bool has_nested_type = false;
	for (size_t i = 0; i < decl.specifiers.size(); i++)
		if (decl.specifiers[i].kind == SPEC_NESTED_DECL)
			has_nested_type = true;
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (decl.declarators.empty())
	{
		// Legal only when the specifier itself declared a class or
		// enum (already bound while processing the specifiers).
		if (!has_nested_type)
			throw runtime_error("declaration declares nothing");
		return;
	}
	for (size_t i = 0; i < decl.declarators.size(); i++)
		BindInitDeclarator(specs, decl.declarators[i]);
}

void DeclBinder::BindInitDeclarator(const DeclSpecifierInfo& specs,
                                    const AstInitDeclarator& declarator)
{
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(declarator.declarator.get(), specs.type);
	if (!composed.id)
		throw runtime_error("declarator requires a name");
	if (!composed.id->IsPlainIdentifier())
		throw OutsideBoundary("qualified declarator-id");
	const string& name = composed.id->parts[0].identifier;
	if (specs.is_typedef)
	{
		if (declarator.init)
			throw runtime_error("typedef declarator with an initializer");
		BindTypeAlias(name, composed.type);
		return;
	}
	if (composed.type->kind == TK_FUNCTION)
	{
		if (declarator.init)
			throw OutsideBoundary("function declarator initializer");
		if (ScopeBinding* existing = FindOwnBinding(*current_, name))
		{
			if (existing->kind != SB_FUNCTION)
				throw runtime_error("redeclaration of " + name);
			existing->type =
				MergeRedeclaredType(existing->type, composed.type);
			return;
		}
		ScopeBinding binding;
		binding.kind = SB_FUNCTION;
		binding.name = name;
		binding.type = composed.type;
		AddBinding(*current_, binding);
		return;
	}
	// 7.1.5p9: a constexpr object declaration declares a const object.
	TypePtr type = composed.type;
	if (specs.is_constexpr)
		type = MakeCvQualifiedType(type, true, false);
	BindVariable(name, type, declarator.init.get(), specs.is_static);
}

void DeclBinder::BindVariable(const string& name, const TypePtr& type,
                              const AstInitializer* init, bool is_static)
{
	if (IsVoidType(type))
		throw runtime_error("variable of void type");
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		if (existing->kind != SB_VARIABLE)
			throw runtime_error("redeclaration of " + name);
		existing->type = MergeRedeclaredType(existing->type, type);
		RecordConstantValue(*existing, init);
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_VARIABLE;
	binding.name = name;
	binding.type = type;
	RecordConstantValue(binding, init);
	AddBinding(*current_, binding);
	if (current_->kind == SCOPE_CLASS && current_fields_ && !is_static)
		current_fields_->push_back(type);
}

void DeclBinder::BindTypeAlias(const string& name, const TypePtr& type)
{
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 7.1.3p3: a typedef may redeclare the same type.
		if (existing->kind != SB_TYPE_ALIAS ||
		    !TypeEquals(existing->type, type))
			throw runtime_error("conflicting type alias " + name);
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_TYPE_ALIAS;
	binding.name = name;
	binding.type = type;
	AddBinding(*current_, binding);
}

void DeclBinder::RecordConstantValue(ScopeBinding& binding,
                                     const AstInitializer* init)
{
	if (!init || binding.has_value)
		return;
	if (!binding.type->is_const || !IsIntegralType(binding.type))
		return;
	const AstExpr* expr = 0;
	if (init->kind == INIT_EQ)
		expr = init->expr.get();
	else if (init->kind == INIT_PAREN && init->args.size() == 1)
		expr = init->args[0].get();
	if (!expr)
		return;
	try
	{
		ConstValue value = EvaluateConstExpr(*expr, *this);
		binding.value =
			ConvertConstValue(value, binding.type->fundamental);
		binding.has_value = true;
	}
	catch (const std::exception&)
	{
		// Not a constant expression of the subset: the object simply
		// has no recorded value, and only a later constant context
		// that reads it is an error.
	}
}

void DeclBinder::BindFunctionDefinition(const AstDecl& decl)
{
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (specs.is_typedef)
		throw runtime_error("typedef on a function definition");
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(decl.declarator.get(), specs.type);
	if (!composed.id || !composed.id->IsPlainIdentifier())
		throw OutsideBoundary("qualified function definition");
	if (!composed.declares_function || composed.type->kind != TK_FUNCTION)
		throw runtime_error("function definition requires a function "
		                    "declarator");
	const string& name = composed.id->parts[0].identifier;
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		if (existing->kind != SB_FUNCTION)
			throw runtime_error("redeclaration of " + name);
		existing->type = MergeRedeclaredType(existing->type, composed.type);
	}
	else
	{
		ScopeBinding binding;
		binding.kind = SB_FUNCTION;
		binding.name = name;
		binding.type = composed.type;
		AddBinding(*current_, binding);
	}
	Scope* scope = model_.CreateScope(SCOPE_FUNCTION, name, current_);
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		const ParameterInfo& parameter = composed.parameters[i];
		if (parameter.name.empty())
			continue;
		ScopeBinding binding;
		binding.kind = SB_PARAMETER;
		binding.name = parameter.name;
		binding.type = parameter.type;
		AddBinding(*scope, binding);
	}
	Scope* saved = current_;
	current_ = scope;
	BindStatement(*decl.body);
	current_ = saved;
}

void DeclBinder::BindTemplateDeclaration(const AstDecl& decl)
{
	if (!decl.has_parameter_list || !decl.inner)
		throw OutsideBoundary("explicit specialization");
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "", current_);
	for (size_t i = 0; i < decl.template_params.size(); i++)
	{
		const AstTemplateParameter& parameter = decl.template_params[i];
		// Non-type template parameter binding is outside the PA11
		// boundary; unnamed parameters bind nothing.
		if (parameter.kind == TP_NON_TYPE || parameter.name.empty())
			continue;
		const char* prefix = parameter.kind == TP_TYPE
			? "typename " : "template-parameter ";
		NamedTypeInfo* info =
			model_.CreateNamedTypeInfo(prefix + parameter.name);
		ScopeBinding binding;
		binding.kind = SB_TYPE;
		binding.name = parameter.name;
		binding.type = MakeNamedType(TK_TYPE_PARAM, info);
		AddBinding(*scope, binding);
	}
	Scope* saved = current_;
	current_ = scope;
	BindDeclaration(*decl.inner);
	current_ = saved;
}

// 9.6: bit-field members bind as data members of the declared type
// (their widths do not enter the PA11 model).
void DeclBinder::BindBitFieldDeclaration(const AstDecl& decl)
{
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (specs.is_typedef)
		throw runtime_error("typedef bit-field");
	for (size_t i = 0; i < decl.bit_fields.size(); i++)
	{
		const AstDecl::BitField& field = decl.bit_fields[i];
		if (!field.declarator)
			continue;  // anonymous bit-field declares nothing
		DeclaratorInfo composed =
			builder_.ComposeDeclarator(field.declarator.get(), specs.type);
		if (!composed.id || !composed.id->IsPlainIdentifier())
			throw OutsideBoundary("bit-field declarator");
		if (!IsIntegralType(composed.type))
			throw runtime_error("bit-field of a non-integral type");
		BindVariable(composed.id->parts[0].identifier, composed.type, 0,
		             specs.is_static);
	}
}

// --- classes and enums ----------------------------------------------------

string DeclBinder::AnonymousTypeName(const AstDecl& decl) const
{
	return "__anonymous_" + decl.class_key_spelling + "_type__" +
		to_string(decl.begin_token) + "_" + to_string(decl.end_token);
}

void DeclBinder::CompleteClassLayout(NamedTypeInfo& info,
                                     const vector<TypePtr>& fields)
{
	for (size_t i = 0; i < fields.size(); i++)
	{
		if (LayoutDependent(fields[i]))
		{
			info.size = 0;
			info.alignment = 0;
			return;
		}
	}
	unsigned long long size = 0;
	unsigned long long alignment = 1;
	for (size_t i = 0; i < fields.size(); i++)
	{
		unsigned long long field_size = TypeSize(fields[i]);
		unsigned long long field_alignment = TypeAlignment(fields[i]);
		if (info.is_union)
			size = size > field_size ? size : field_size;
		else
			size = RoundUp(size, field_alignment) + field_size;
		if (field_alignment > alignment)
			alignment = field_alignment;
	}
	size = RoundUp(size, alignment);
	info.size = size ? size : 1;
	info.alignment = alignment;
}

void DeclBinder::InjectAnonymousUnionMembers(const Scope& union_scope)
{
	// 9.5p5: the member names enter the enclosing scope as variables.
	for (size_t i = 0; i < union_scope.bindings.size(); i++)
	{
		const ScopeBinding& member = union_scope.bindings[i];
		if (member.kind != SB_VARIABLE)
			throw runtime_error("anonymous union member is not a "
			                    "non-static data member");
		AddBinding(*current_, member);
	}
}

TypePtr DeclBinder::BindClass(const AstDecl& decl, bool standalone)
{
	if (!decl.bases.empty())
		throw OutsideBoundary("base classes");
	bool is_union = decl.class_key == KW_UNION;
	bool anonymous = !decl.has_name;
	string name;
	if (anonymous)
		name = AnonymousTypeName(decl);
	else
	{
		if (!decl.class_name.IsPlainIdentifier())
			throw OutsideBoundary("qualified or template class-name");
		name = decl.class_name.parts[0].identifier;
	}
	if (standalone && anonymous && !is_union)
		throw runtime_error("anonymous class declaration declares nothing");

	NamedTypeInfo* info;
	TypePtr type;
	ScopeBinding* existing =
		anonymous ? 0 : FindOwnBinding(*current_, name);
	if (existing)
	{
		if (existing->kind != SB_TYPE ||
		    existing->type->kind != TK_CLASS)
			throw runtime_error(name + " redeclared as a class");
		info = model_.MutableInfo(existing->type->named);
		if (info->is_union != is_union)
			throw runtime_error("class-key disagrees with " + name);
		if (info->complete)
			throw runtime_error("redefinition of " + name);
		type = existing->type;
	}
	else
	{
		info = model_.CreateNamedTypeInfo(
			decl.class_key_spelling + " " + name);
		info->is_union = is_union;
		type = MakeNamedType(TK_CLASS, info);
		if (!anonymous)
		{
			ScopeBinding binding;
			binding.kind = SB_TYPE;
			binding.name = name;
			binding.type = type;
			AddBinding(*current_, binding);
		}
	}

	Scope* scope = model_.CreateScope(SCOPE_CLASS, name, current_);
	model_.SetMemberScope(info, scope);
	vector<TypePtr> fields;
	Scope* saved_scope = current_;
	vector<TypePtr>* saved_fields = current_fields_;
	current_ = scope;
	current_fields_ = &fields;
	BindDeclarations(decl.members);
	current_ = saved_scope;
	current_fields_ = saved_fields;
	info->complete = true;
	CompleteClassLayout(*info, fields);

	if (standalone && anonymous)
	{
		InjectAnonymousUnionMembers(*scope);
		if (current_->kind == SCOPE_CLASS && current_fields_)
			current_fields_->push_back(type);
	}
	return type;
}

TypePtr DeclBinder::BindClassForward(const AstDecl& decl, bool elaborated)
{
	if (!decl.class_name.IsPlainIdentifier())
		throw OutsideBoundary("qualified or template class-name");
	const string& name = decl.class_name.parts[0].identifier;
	bool is_union = decl.class_key == KW_UNION;
	// 3.4.4p2 for the elaborated specifier form (any visible class);
	// a standalone forward declaration declares in the current scope.
	const ScopeBinding* found = elaborated
		? UnqualifiedLookup(current_, name, SLF_SCOPE_NAMES)
		: FindOwnBinding(*current_, name);
	if (found)
	{
		if (found->kind != SB_TYPE || found->type->kind != TK_CLASS ||
		    found->type->named->is_union != is_union)
			throw runtime_error(name + " does not name a matching class");
		return found->type;
	}
	NamedTypeInfo* info =
		model_.CreateNamedTypeInfo(decl.class_key_spelling + " " + name);
	info->is_union = is_union;
	TypePtr type = MakeNamedType(TK_CLASS, info);
	ScopeBinding binding;
	binding.kind = SB_TYPE;
	binding.name = name;
	binding.type = type;
	AddBinding(*current_, binding);
	return type;
}

TypePtr DeclBinder::DeclareEnumEntity(const AstDecl& decl,
                                      const string& name, bool scoped,
                                      const TypePtr& underlying)
{
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 7.2p2-p4: every redeclaration must agree on scoped-ness and
		// underlying type.
		if (existing->kind != SB_TYPE ||
		    existing->type->kind != TK_ENUM)
			throw runtime_error(name + " redeclared as an enumeration");
		EnumFacts& facts = enum_facts_[existing->type->named];
		if (facts.scoped != scoped ||
		    !TypeEquals(facts.underlying, underlying))
			throw runtime_error("conflicting redeclaration of enum " +
			                    name);
		return existing->type;
	}
	NamedTypeInfo* info = model_.CreateNamedTypeInfo(
		(scoped ? "enum class " : "enum ") + name);
	info->complete = true;
	info->size = TypeSize(underlying);
	info->alignment = TypeAlignment(underlying);
	TypePtr type = MakeNamedType(TK_ENUM, info);
	EnumFacts& facts = enum_facts_[info];
	facts.underlying = underlying;
	facts.scoped = scoped;
	if (!decl.name.empty())
	{
		ScopeBinding binding;
		binding.kind = SB_TYPE;
		binding.name = name;
		binding.type = type;
		AddBinding(*current_, binding);
	}
	// Scoped enumerations own a member scope from their first
	// declaration (the fixtures print it even for opaque declarations).
	if (scoped)
		model_.SetMemberScope(
			info, model_.CreateScope(SCOPE_ENUM, name, current_));
	return type;
}

void DeclBinder::BindEnumerators(const AstDecl& decl,
                                 const TypePtr& enum_type)
{
	const EnumFacts& facts = enum_facts_[enum_type->named];
	EFundamentalType underlying = facts.underlying->fundamental;
	Scope* target = facts.scoped
		? model_.MemberScope(enum_type->named) : current_;
	Scope* saved = current_;
	// 7.2p10: earlier enumerators are in scope inside the list.
	current_ = target;
	ConstValue next(underlying, 0);
	for (size_t i = 0; i < decl.enumerators.size(); i++)
	{
		const AstEnumerator& enumerator = decl.enumerators[i];
		ConstValue value = next;
		if (enumerator.value)
			value = ConvertConstValue(
				EvaluateConstExpr(*enumerator.value, *this), underlying);
		ScopeBinding binding;
		binding.kind = SB_ENUMERATOR;
		binding.name = enumerator.name;
		binding.type = enum_type;
		binding.has_value = true;
		binding.value = value;
		AddBinding(*target, binding);
		next = ConvertConstValue(
			ConstValue(underlying, value.bits + 1), underlying);
	}
	current_ = saved;
}

TypePtr DeclBinder::BindEnum(const AstDecl& decl)
{
	bool scoped = decl.has_enum_key;
	string name = decl.name;
	if (name.empty())
	{
		if (scoped || !decl.enum_body)
			throw runtime_error("anonymous enumeration form");
		name = "__anonymous_enum" + to_string(++anonymous_enums_);
	}
	TypePtr underlying;
	if (decl.has_enum_base)
	{
		underlying = RemoveTopCv(builder_.ResolveTypeId(*decl.type));
		if (!IsIntegralType(underlying))
			throw runtime_error("enum underlying type must be integral");
	}
	else if (scoped || decl.enum_body)
		// 7.2p5 for scoped enumerations; for unscoped definitions the
		// PA11 model fixes int rather than computing a value-dependent
		// type.
		underlying = MakeFundamentalType(FT_INT);
	else
		// 7.2p2: an opaque unscoped declaration requires a fixed base.
		throw runtime_error("opaque unscoped enum declaration");

	TypePtr type = DeclareEnumEntity(decl, name, scoped, underlying);
	if (decl.enum_body)
	{
		EnumFacts& facts = enum_facts_[type->named];
		if (facts.defined)
			throw runtime_error("redefinition of enum " + name);
		facts.defined = true;
		BindEnumerators(decl, type);
	}
	return type;
}

// --- statements ------------------------------------------------------------

// Statement semantics are out of scope for PA11; the walk only creates
// nested block scopes and binds local declarations.
void DeclBinder::BindStatement(const AstStmt& stmt)
{
	switch (stmt.kind)
	{
	case SK_COMPOUND:
	{
		Scope* saved = current_;
		current_ = model_.CreateScope(SCOPE_BLOCK, "", saved);
		for (size_t i = 0; i < stmt.items.size(); i++)
			BindStatement(*stmt.items[i]);
		current_ = saved;
		return;
	}
	case SK_DECLARATION:
		BindDeclaration(*stmt.decl);
		return;
	case SK_IF:
		if (stmt.then_branch)
			BindStatement(*stmt.then_branch);
		if (stmt.else_branch)
			BindStatement(*stmt.else_branch);
		return;
	case SK_FOR:
		if (stmt.for_init)
			BindStatement(*stmt.for_init);
		if (stmt.body)
			BindStatement(*stmt.body);
		return;
	case SK_TRY:
		if (stmt.body)
			BindStatement(*stmt.body);
		for (size_t i = 0; i < stmt.handlers.size(); i++)
			if (stmt.handlers[i].body)
				BindStatement(*stmt.handlers[i].body);
		return;
	case SK_SWITCH:
	case SK_WHILE:
	case SK_DO:
	case SK_LABELED:
	case SK_CASE:
	case SK_DEFAULT:
		if (stmt.body)
			BindStatement(*stmt.body);
		return;
	default:
		return;  // expression and jump statements
	}
}
