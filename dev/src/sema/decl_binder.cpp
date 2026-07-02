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

// True when the two typed values denote the same integer: the stored
// 64 bits agree and so does their sign interpretation.
bool SameIntegerValue(const ConstValue& a, const ConstValue& b)
{
	bool a_negative = IsSignedIntegralFundamental(a.type) &&
		(long long)a.bits < 0;
	bool b_negative = IsSignedIntegralFundamental(b.type) &&
		(long long)b.bits < 0;
	return a.bits == b.bits && a_negative == b_negative;
}

// 7.2p5: the implicit value of an enumerator without an initializer is
// one more than the previous enumerator, and must be representable in
// the underlying type.
ConstValue SuccessorValue(const ConstValue& value, EFundamentalType type)
{
	bool is_signed = IsSignedIntegralFundamental(type);
	if (value.bits == (is_signed ? 0x7fffffffffffffffull : ~0ull))
		throw runtime_error("enumerator value overflows the underlying "
		                    "type");
	ConstValue next = ConvertConstValue(ConstValue(type, value.bits + 1),
	                                    type);
	if (next.bits != value.bits + 1)
		throw runtime_error("enumerator value overflows the underlying "
		                    "type");
	return next;
}

}  // namespace

DeclBinder::DeclBinder(TypesModel& model)
	: model_(model), builder_(*this), current_(model.global()),
	  current_fields_(0), anonymous_enums_(0), in_c_linkage_(false),
	  current_access_(MA_PUBLIC),
	  allow_qualified_class_name_(false)
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

string DeclBinder::DeclaredFunctionName(const AstNamePart& part)
{
	if (part.kind == NP_OPERATOR_FUNCTION)
		return "operator " + part.operator_text;
	if (part.kind == NP_LITERAL_OPERATOR)
		return "operator \"\"" + part.identifier;
	return PartName(part);
}

void DeclBinder::RecordFunctionFacts(ScopeBinding& binding,
                                     const DeclaratorInfo& composed,
                                     bool deleted,
                                     const DeclSpecifierInfo* specs,
                                     bool inline_def, bool adl_only)
{
	// Locate the overload slot whose parameter list matches this
	// declaration (entry 0 is binding.type).
	size_t index = 0;
	for (size_t i = 0; i < binding.overloads.size(); i++)
		if (TypeEquals(binding.overloads[i], composed.type))
			index = i + 1;
	size_t old_count = binding.fn_defaults.size();
	size_t count = binding.overloads.size() + 1;
	binding.fn_defaults.resize(count);
	binding.fn_deleted.resize(count, false);
	binding.fn_access.resize(count, MA_PUBLIC);
	binding.fn_static.resize(count, false);
	binding.fn_inline_def.resize(count, false);
	binding.fn_adl_only.resize(count, false);
	binding.fn_unwind_no.resize(count, false);
	binding.fn_owner.resize(count, 0);
	if (binding.home)
		binding.fn_owner[index] = binding.home;
	if (deleted)
		binding.fn_deleted[index] = true;
	if (index >= old_count)
	{
		binding.fn_access[index] = current_access_;
		// 11.3/7.3.1.2p3: a name declared only by friend declarations
		// is invisible to ordinary lookup; any ordinary declaration
		// makes it visible.
		binding.fn_adl_only[index] = adl_only;
	}
	else if (!adl_only)
		binding.fn_adl_only[index] = false;
	if (specs && specs->is_static)
		binding.fn_static[index] = true;
	if (inline_def)
		binding.fn_inline_def[index] = true;
	if (composed.noexcept_simple)
		binding.fn_unwind_no[index] = true;
	// 8.3.6p4: later declarations may add default arguments.
	vector<const AstExpr*>& defaults = binding.fn_defaults[index];
	defaults.resize(composed.parameters.size(), 0);
	for (size_t i = 0; i < composed.parameters.size(); i++)
		if (composed.parameters[i].default_arg && !defaults[i])
			defaults[i] = composed.parameters[i].default_arg;
	// PA18: remember declared parameter names (first non-empty wins).
	binding.fn_param_names.resize(count);
	vector<string>& names = binding.fn_param_names[index];
	names.resize(composed.parameters.size());
	for (size_t i = 0; i < composed.parameters.size(); i++)
		if (names[i].empty())
			names[i] = composed.parameters[i].name;
}

string DeclBinder::TerminalName(const AstName& name)
{
	if (name.parts.empty())
		throw OutsideBoundary("name form");
	const AstNamePart& part = name.parts.back();
	if (part.kind == NP_OPERATOR_FUNCTION ||
	    part.kind == NP_LITERAL_OPERATOR)
		return DeclaredFunctionName(part);
	return PartName(part);
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
		EnsureTypeCompleteness(type->named);
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
		// PA18: a template-id component instantiates its class template.
		if (name.parts[0].kind == NP_TEMPLATE_ID)
		{
			scope = ScopeOfBinding(
				*ResolveTemplateIdBinding(name.parts[0], 0));
			index = 1;
		}
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
	}
	for (; index + 1 < name.parts.size(); index++)
	{
		if (name.parts[index].kind == NP_TEMPLATE_ID)
		{
			scope = ScopeOfBinding(
				*ResolveTemplateIdBinding(name.parts[index], scope));
			continue;
		}
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
	// PA18: a terminal template-id resolves through the instantiation
	// seam (class templates; function template-ids are handled by the
	// expression layer before reaching here).
	if (name.parts.back().kind == NP_TEMPLATE_ID)
	{
		Scope* prefix = 0;
		if (name.parts.size() > 1 || name.global_scope)
			prefix = ResolvePrefixScope(name);
		return ResolveTemplateIdBinding(name.parts.back(), prefix);
	}
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
		return;
	case DK_ACCESS_LABEL:
		current_access_ = decl.access == KW_PRIVATE ? MA_PRIVATE
			: decl.access == KW_PROTECTED ? MA_PROTECTED : MA_PUBLIC;
		return;
	// Special members do not enter the PA11 declaration model; the
	// PA15 binder overrides the seam.
	case DK_SPECIAL_MEMBER_DECLARATION:
	case DK_SPECIAL_MEMBER_DEFINITION:
		BindSpecialMember(decl);
		return;
	case DK_TRANSLATION_UNIT:
		BindDeclarations(decl.body_decls);
		return;
	case DK_LINKAGE:
	{
		// 7.5: members of an extern "C" body get C language linkage;
		// the scope model itself is unaffected.
		bool saved = in_c_linkage_;
		in_c_linkage_ = saved || decl.linkage == "C";
		BindDeclarations(decl.body_decls);
		in_c_linkage_ = saved;
		return;
	}
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
		BindClassDeclaration(decl);
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
		BindExplicitInstantiation(decl);
		return;
	}
	throw OutsideBoundary("declaration form");
}

void DeclBinder::BindClassDeclaration(const AstDecl& decl)
{
	BindClass(decl, true);
}

void DeclBinder::EnsureTypeCompleteness(const NamedTypeInfo* info)
{
	(void)info;
}

void DeclBinder::BindExplicitInstantiation(const AstDecl& decl)
{
	(void)decl;
	throw OutsideBoundary("explicit instantiation");
}

void DeclBinder::RequireCompleteForLayout(const TypePtr& type)
{
	TypePtr bare = type;
	while (bare && bare->kind == TK_ARRAY)
		bare = bare->target;
	if (bare && bare->kind == TK_CLASS)
		EnsureTypeCompleteness(bare->named);
}

const ScopeBinding* DeclBinder::ResolveTemplateIdBinding(
	const AstNamePart& part, Scope* prefix)
{
	(void)part;
	(void)prefix;
	throw OutsideBoundary("template-id name");
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
	BindNamespaceBody(decl, scope);
}

void DeclBinder::BindNamespaceBody(const AstDecl& decl, Scope* scope)
{
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
	Scope* prefix = ResolvePrefixScope(target);
	if (prefix->kind == SCOPE_CLASS && prefix->name == name)
	{
		// 12.9: `using Base::Base` inherits the base constructors.
		BindInheritingConstructors(prefix);
		return;
	}
	const ScopeBinding* found = QualifiedLookup(*prefix, name, SLF_ANY);
	if (!found)
		throw runtime_error("using-declaration target not found: " + name);
	if (found->kind == SB_NAMESPACE || found->kind == SB_NAMESPACE_ALIAS)
		throw runtime_error("using-declaration shall not name a namespace");
	// The imported binding shares the entity's type, value, and member
	// scope, and prints in the current scope under its own kind. In a
	// class, the import re-exposes the member under the current access.
	ScopeBinding imported = *found;
	if (current_->kind == SCOPE_CLASS)
	{
		imported.access = current_access_;
		for (size_t i = 0; i < imported.fn_access.size(); i++)
			imported.fn_access[i] = current_access_;
	}
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 7.3.3p15: the class's own function declarations and the
		// imported overloads form one set.
		if (current_->kind != SCOPE_CLASS ||
		    existing->kind != SB_FUNCTION ||
		    imported.kind != SB_FUNCTION)
			throw runtime_error("redeclaration of " + name);
		MergeImportedOverloads(*existing, imported);
		return;
	}
	AddBinding(*current_, imported);
}

void DeclBinder::BindInheritingConstructors(Scope* base_scope)
{
	(void)base_scope;
	throw OutsideBoundary("inheriting constructors");
}

// Appends the imported overloads that no own declaration replaces
// (an own member with the same parameter list wins, 7.3.3p15).
void DeclBinder::MergeImportedOverloads(ScopeBinding& own,
                                        const ScopeBinding& imported)
{
	vector<TypePtr> incoming;
	incoming.push_back(imported.type);
	for (size_t i = 0; i < imported.overloads.size(); i++)
		incoming.push_back(imported.overloads[i]);
	for (size_t i = 0; i < incoming.size(); i++)
	{
		bool replaced = TypeEquals(RemoveTopCv(own.type),
		                           RemoveTopCv(incoming[i]));
		for (size_t j = 0; !replaced && j < own.overloads.size(); j++)
			if (TypeEquals(RemoveTopCv(own.overloads[j]),
			               RemoveTopCv(incoming[i])))
				replaced = true;
		if (replaced)
			continue;
		own.overloads.push_back(incoming[i]);
		size_t count = own.overloads.size() + 1;
		own.fn_defaults.resize(count);
		own.fn_deleted.resize(count, false);
		own.fn_access.resize(count, MA_PUBLIC);
		own.fn_static.resize(count, false);
		own.fn_inline_def.resize(count, false);
		own.fn_adl_only.resize(count, false);
		own.fn_unwind_no.resize(count, false);
		own.fn_owner.resize(count, 0);
		size_t at = count - 1;
		own.fn_access[at] = current_access_;
		own.fn_owner[at] =
			i < imported.fn_owner.size() && imported.fn_owner[i]
				? imported.fn_owner[i] : imported.owner;
		if (i < imported.fn_defaults.size())
			own.fn_defaults[at] = imported.fn_defaults[i];
		if (i < imported.fn_deleted.size())
			own.fn_deleted[at] = imported.fn_deleted[i];
		if (i < imported.fn_static.size())
			own.fn_static[at] = imported.fn_static[i];
		if (i < imported.fn_inline_def.size())
			own.fn_inline_def[at] = imported.fn_inline_def[i];
		if (i < imported.fn_unwind_no.size())
			own.fn_unwind_no[at] = imported.fn_unwind_no[i];
	}
}

void DeclBinder::BindStaticAssert(const AstDecl& decl)
{
	if (!ConstValueIsNonZero(EvaluateConstExpr(*decl.assert_expr, *this)))
		throw runtime_error("static_assert failed " + decl.message);
}

bool DeclBinder::HasFriendSpecifier(const AstDecl& decl)
{
	for (size_t i = 0; i < decl.specifiers.size(); i++)
		if (decl.specifiers[i].kind == SPEC_KEYWORD &&
		    decl.specifiers[i].keyword == KW_FRIEND)
			return true;
	return false;
}

void DeclBinder::BindFriendDeclaration(const AstDecl& decl)
{
	(void)decl;
	throw OutsideBoundary("friend declaration");
}

void DeclBinder::BindSimpleDeclaration(const AstDecl& decl)
{
	if (HasFriendSpecifier(decl))
	{
		BindFriendDeclaration(decl);
		return;
	}
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
	if (composed.id->parts.size() != 1 || composed.id->global_scope)
	{
		BindQualifiedDeclarator(specs, declarator, composed);
		return;
	}
	if (!composed.id->IsPlainIdentifier() &&
	    composed.type->kind != TK_FUNCTION)
		throw OutsideBoundary("declarator-id form");
	const string name = DeclaredFunctionName(composed.id->parts[0]);
	if (specs.is_typedef)
	{
		if (declarator.init)
			throw runtime_error("typedef declarator with an initializer");
		BindTypeAlias(name, composed.type);
		return;
	}
	// 7.1.2p5 / 9.2: `virtual` and the virt-specifiers belong to member
	// function declarations inside a class body (the PA17 binder checks
	// their meaning there).
	if ((composed.type->kind != TK_FUNCTION ||
	     current_->kind != SCOPE_CLASS) &&
	    (specs.is_virtual || composed.has_override || composed.has_final))
		throw runtime_error("virtual declaration outside a class "
		                    "member function");
	if (composed.type->kind == TK_FUNCTION)
	{
		// 8.4.3: `= delete` is a deleted definition; `= default` is a
		// defaulted definition of a special member function; `= 0` on a
		// member function declarator is the pure-specifier (9.2p1); any
		// other initializer on a function declarator is ill-formed.
		bool deleted = declarator.init &&
			declarator.init->kind == INIT_DELETE;
		bool defaulted = declarator.init &&
			declarator.init->kind == INIT_DEFAULT;
		bool pure = current_->kind == SCOPE_CLASS && declarator.init &&
			declarator.init->kind == INIT_EQ && declarator.init->expr &&
			declarator.init->expr->kind == EK_LITERAL &&
			declarator.init->expr->literal == "0";
		if (defaulted &&
		    (current_->kind != SCOPE_CLASS || name != "operator ="))
			throw runtime_error("defaulted non-special member function");
		if (declarator.init && !deleted && !defaulted && !pure)
			throw OutsideBoundary("function declarator initializer");
		// 8.3.5p6: cv-qualifiers only on non-static member functions.
		if ((composed.type->is_const || composed.type->is_volatile) &&
		    current_->kind != SCOPE_CLASS)
			throw runtime_error("cv-qualified non-member function");
		ScopeBinding& binding = BindFunctionName(name, composed.type, true);
		RecordFunctionFacts(binding, composed, deleted, &specs);
		if (defaulted)
		{
			size_t index = 0;
			for (size_t i = 0; i < binding.overloads.size(); i++)
				if (TypeEquals(binding.overloads[i], composed.type))
					index = i + 1;
			binding.fn_defaulted.resize(
				binding.overloads.size() + 1, false);
			binding.fn_defaulted[index] = true;
		}
		if (in_c_linkage_)
			binding.c_linkage = true;
		OnFunctionDeclared(binding, composed.type, &specs, &composed,
		                   pure);
		return;
	}
	// 7.1.5p9: a constexpr object declaration declares a const object.
	TypePtr type = composed.type;
	if (specs.is_constexpr)
		type = MakeCvQualifiedType(type, true, false);
	BindVariable(name, type, declarator.init.get(), specs);
}

void DeclBinder::BindVariable(const string& name, const TypePtr& type,
                              const AstInitializer* init,
                              const DeclSpecifierInfo& specs)
{
	if (IsVoidType(type))
		throw runtime_error("variable of void type");
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 3.3.1/9.2p1: an object redeclares in a namespace scope or
		// through extern in a block scope; a data member or plain local
		// shall not be declared twice.
		if (existing->kind != SB_VARIABLE ||
		    (current_->kind != SCOPE_NAMESPACE &&
		     !(current_->kind == SCOPE_BLOCK && specs.is_extern)))
			throw runtime_error("redeclaration of " + name);
		existing->type = MergeRedeclaredType(existing->type, type);
		RecordConstantValue(*existing, init);
		OnVariableBound(*existing, init, specs);
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_VARIABLE;
	binding.name = name;
	binding.type = type;
	binding.access = current_access_;
	binding.is_mutable = specs.is_mutable;
	binding.is_thread_local = specs.is_thread_local;
	RecordConstantValue(binding, init);
	ScopeBinding& added = AddBinding(*current_, binding);
	if (current_->kind == SCOPE_CLASS && current_fields_ && !specs.is_static)
		current_fields_->push_back(type);
	OnVariableBound(added, init, specs);
}

void DeclBinder::BindTypeAlias(const string& name, const TypePtr& type)
{
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 7.1.3p3-p4: a typedef-name may redeclare the same type - any
		// such name in a non-class scope, but in a class scope only the
		// name of a class or enumeration declared there (9.2p1).
		bool redeclarable = existing->kind == SB_TYPE ||
			(existing->kind == SB_TYPE_ALIAS &&
			 current_->kind != SCOPE_CLASS);
		if (!redeclarable || !TypeEquals(existing->type, type))
			throw runtime_error("conflicting type alias " + name);
		OnTypeAliasBound(name, type);
		return;
	}
	ScopeBinding binding;
	binding.kind = SB_TYPE_ALIAS;
	binding.name = name;
	binding.type = type;
	binding.access = current_access_;
	AddBinding(*current_, binding);
	OnTypeAliasBound(name, type);
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

void DeclBinder::BindQualifiedDeclarator(const DeclSpecifierInfo& specs,
                                         const AstInitDeclarator& declarator,
                                         const DeclaratorInfo& composed)
{
	(void)specs;
	(void)declarator;
	(void)composed;
	throw OutsideBoundary("qualified declarator-id");
}

void DeclBinder::BindFunctionDefinition(const AstDecl& decl)
{
	if (HasFriendSpecifier(decl))
	{
		BindFriendDeclaration(decl);
		return;
	}
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (specs.is_typedef)
		throw runtime_error("typedef on a function definition");
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(decl.declarator.get(), specs.type);
	if (!composed.id || composed.id->parts.empty())
		throw OutsideBoundary("function definition declarator");
	if (!composed.declares_function || composed.type->kind != TK_FUNCTION)
		throw runtime_error("function definition requires a function "
		                    "declarator");
	const string name =
		DeclaredFunctionName(composed.id->parts.back());
	// 8.3p1: a qualified declarator-id defines an already-declared
	// member of the named (namespace) scope.
	Scope* declaring = current_;
	if (composed.id->parts.size() > 1 || composed.id->global_scope)
	{
		declaring = ResolvePrefixScope(*composed.id);
		CheckQualifiedDefinitionScope(declaring);
		if (!FindOwnBinding(*declaring, name))
			throw runtime_error(name + " is not a member of the "
			                    "named scope");
	}
	// 8.3.5p6: cv-qualifiers only on non-static member functions.
	if ((composed.type->is_const || composed.type->is_volatile) &&
	    declaring->kind != SCOPE_CLASS)
		throw runtime_error("cv-qualified non-member function");
	// 7.1.2p5 / 9.2: only a member function defined inside its class
	// body may spell `virtual` or a virt-specifier; out-of-class and
	// namespace-scope definitions cannot respell them.
	if ((declaring != current_ || declaring->kind != SCOPE_CLASS) &&
	    (specs.is_virtual || composed.has_override || composed.has_final))
		throw runtime_error("virtual definition outside a class body");
	Scope* saved = current_;
	current_ = declaring;
	ScopeBinding& binding = BindFunctionName(name, composed.type, false);
	RecordFunctionFacts(binding, composed, false, &specs,
	                    declaring->kind == SCOPE_CLASS);
	if (in_c_linkage_)
		binding.c_linkage = true;
	// PA17: an in-class member function definition declares the member;
	// the binder records virtual-slot facts through the same event.
	if (declaring->kind == SCOPE_CLASS)
		OnFunctionDeclared(binding, composed.type, &specs, &composed,
		                   false);
	Scope* scope = model_.CreateScope(SCOPE_FUNCTION, name, declaring);
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		const ParameterInfo& parameter = composed.parameters[i];
		if (parameter.name.empty())
			continue;
		ScopeBinding param_binding;
		param_binding.kind = SB_PARAMETER;
		param_binding.name = parameter.name;
		param_binding.type = parameter.type;
		AddBinding(*scope, param_binding);
	}
	current_ = scope;
	BindFunctionBody(decl, composed, name);
	current_ = saved;
}

ScopeBinding& DeclBinder::BindFunctionName(const string& name,
                                           const TypePtr& type,
                                           bool allow_block)
{
	if (ScopeBinding* existing = FindOwnBinding(*current_, name))
	{
		// 3.3.1/9.2p1: functions redeclare in namespace scopes (and, for
		// plain declarations, block scopes); a member shall not be
		// declared twice.
		if (existing->kind != SB_FUNCTION ||
		    (current_->kind != SCOPE_NAMESPACE &&
		     !(allow_block && current_->kind == SCOPE_BLOCK)))
			throw runtime_error("redeclaration of " + name);
		existing->type = MergeRedeclaredType(existing->type, type);
		return *existing;
	}
	ScopeBinding binding;
	binding.kind = SB_FUNCTION;
	binding.name = name;
	binding.type = type;
	binding.access = current_access_;
	return AddBinding(*current_, binding);
}

void DeclBinder::BindFunctionBody(const AstDecl& decl,
                                  const DeclaratorInfo& composed,
                                  const string& name)
{
	(void)composed;
	(void)name;
	BindStatement(*decl.body);
}

void DeclBinder::CheckQualifiedDefinitionScope(const Scope* declaring)
{
	if (declaring->kind != SCOPE_NAMESPACE)
		throw OutsideBoundary("member function definition");
}

void DeclBinder::OnTypeAliasBound(const string& name, const TypePtr& type)
{
	(void)name;
	(void)type;
}

void DeclBinder::OnVariableBound(ScopeBinding& binding,
                                 const AstInitializer* init,
                                 const DeclSpecifierInfo& specs)
{
	(void)binding;
	(void)init;
	(void)specs;
}

void DeclBinder::OnFunctionDeclared(ScopeBinding& binding,
                                    const TypePtr& type,
                                    const DeclSpecifierInfo* specs,
                                    const DeclaratorInfo* composed,
                                    bool pure)
{
	(void)binding;
	(void)type;
	(void)specs;
	(void)composed;
	(void)pure;
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
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			prefix + parameter.name, scope, parameter.name);
		ScopeBinding binding;
		binding.kind = SB_TYPE;
		binding.access = current_access_;
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
		             specs);
	}
}

// --- classes and enums ----------------------------------------------------

string DeclBinder::AnonymousTypeName(const AstDecl& decl)
{
	return "__anonymous_" + decl.class_key_spelling + "_type__" +
		to_string(decl.begin_token) + "_" + to_string(decl.end_token);
}

string DeclBinder::TypeDisplayName(const string& key,
                                   const string& name) const
{
	return key + " " + name;
}

void DeclBinder::CompleteClassLayout(NamedTypeInfo& info,
                                     const vector<TypePtr>& fields,
                                     unsigned long long min_alignment)
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
	if (min_alignment > alignment)
		alignment = min_alignment;
	size = RoundUp(size, alignment);
	info.size = size ? size : 1;
	info.alignment = alignment;
}

unsigned long long DeclBinder::RequestedAlignment(const AstDecl& decl)
{
	unsigned long long strictest = 0;
	for (size_t i = 0; i < decl.align_types.size(); i++)
	{
		unsigned long long value = TypeAlignment(
			RemoveTopCv(builder_.ResolveTypeId(*decl.align_types[i])));
		if (value > strictest)
			strictest = value;
	}
	for (size_t i = 0; i < decl.align_exprs.size(); i++)
	{
		ConstValue value =
			EvaluateConstExpr(*decl.align_exprs[i], *this);
		// 7.6.2p5: a power of two; zero has no effect.
		if (value.bits & (value.bits - 1))
			throw runtime_error("alignment is not a power of two");
		if (value.bits > strictest)
			strictest = value.bits;
	}
	return strictest;
}

void DeclBinder::BindAnonymousUnionMembers(const AstDecl& decl,
                                           const TypePtr& type,
                                           const Scope& union_scope)
{
	(void)decl;
	(void)type;
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

void DeclBinder::BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
                                Scope* scope)
{
	(void)decl;
	(void)info;
	(void)scope;
	throw OutsideBoundary("base classes");
}

void DeclBinder::OnClassOpened(const AstDecl& decl, NamedTypeInfo* info,
                               Scope* scope)
{
	(void)decl;
	(void)info;
	(void)scope;
}

void DeclBinder::CompleteClass(const AstDecl& decl, NamedTypeInfo* info,
                               Scope* scope, const vector<TypePtr>& fields)
{
	(void)scope;
	CompleteClassLayout(*info, fields, RequestedAlignment(decl));
}

void DeclBinder::BindSpecialMember(const AstDecl& decl)
{
	(void)decl;
}

TypePtr DeclBinder::BindClass(const AstDecl& decl, bool standalone)
{
	bool is_union = decl.class_key == KW_UNION;
	bool anonymous = !decl.has_name;
	string name;
	if (anonymous)
		name = AnonymousTypeName(decl);
	else if (decl.class_name.IsPlainIdentifier())
		name = decl.class_name.parts[0].identifier;
	else if (allow_qualified_class_name_)
		// PA18 out-of-class nested definition: the seam resolved the
		// qualified prefix into `current_` already.
		name = PartName(decl.class_name.parts.back());
	else
		throw OutsideBoundary("qualified or template class-name");
	if (standalone && anonymous && !is_union &&
	    current_->kind != SCOPE_CLASS)
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
			TypeDisplayName(decl.class_key_spelling, name), current_,
			name);
		info->is_union = is_union;
		info->class_key = decl.class_key_spelling;
		type = MakeNamedType(TK_CLASS, info);
		if (!anonymous)
		{
			ScopeBinding binding;
			binding.kind = SB_TYPE;
			binding.access = current_access_;
			binding.name = name;
			binding.type = type;
			AddBinding(*current_, binding);
		}
	}

	Scope* scope = model_.CreateScope(SCOPE_CLASS, name, current_);
	model_.SetMemberScope(info, scope);
	OnClassOpened(decl, info, scope);
	if (!decl.bases.empty())
		BindBaseClause(decl, info, scope);
	vector<TypePtr> fields;
	Scope* saved_scope = current_;
	vector<TypePtr>* saved_fields = current_fields_;
	EMemberAccess saved_access = current_access_;
	current_ = scope;
	current_fields_ = &fields;
	current_access_ = decl.class_key == KW_CLASS ? MA_PRIVATE : MA_PUBLIC;
	BindDeclarations(decl.members);
	current_ = saved_scope;
	current_fields_ = saved_fields;
	current_access_ = saved_access;
	// Layout runs before the entity completes so a by-value member of
	// the class itself still reads as incomplete (9.2p9) and fails in
	// TypeSize rather than completing with no layout.
	CompleteClass(decl, info, scope, fields);
	info->complete = true;

	if (standalone && anonymous)
	{
		BindAnonymousUnionMembers(decl, type, *scope);
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
	NamedTypeInfo* info = model_.CreateNamedTypeInfo(
		TypeDisplayName(decl.class_key_spelling, name), current_, name);
	info->is_union = is_union;
	info->class_key = decl.class_key_spelling;
	TypePtr type = MakeNamedType(TK_CLASS, info);
	ScopeBinding binding;
	binding.kind = SB_TYPE;
	binding.access = current_access_;
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
		const NamedTypeInfo* info = existing->type->named;
		if (info->is_scoped != scoped ||
		    info->enum_underlying != underlying->fundamental)
			throw runtime_error("conflicting redeclaration of enum " +
			                    name);
		return existing->type;
	}
	NamedTypeInfo* info = model_.CreateNamedTypeInfo(
		TypeDisplayName(scoped ? "enum class" : "enum", name), current_,
		name);
	info->complete = true;
	info->is_scoped = scoped;
	info->enum_underlying = underlying->fundamental;
	info->size = TypeSize(underlying);
	info->alignment = TypeAlignment(underlying);
	TypePtr type = MakeNamedType(TK_ENUM, info);
	if (!decl.name.empty())
	{
		ScopeBinding binding;
		binding.kind = SB_TYPE;
		binding.access = current_access_;
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
	EFundamentalType underlying = enum_type->named->enum_underlying;
	Scope* target = enum_type->named->is_scoped
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
		{
			// 7.2p5: the given value must be representable in the
			// (PA11 int-fixed) underlying type; silently wrapping
			// would dump a value the program never wrote.
			ConstValue given = EvaluateConstExpr(*enumerator.value, *this);
			value = ConvertConstValue(given, underlying);
			if (!SameIntegerValue(given, value))
				throw runtime_error(enumerator.name + " is not "
				                    "representable in the underlying type");
		}
		ScopeBinding binding;
		binding.kind = SB_ENUMERATOR;
		binding.access = current_access_;
		binding.name = enumerator.name;
		binding.type = enum_type;
		binding.has_value = true;
		binding.value = value;
		AddBinding(*target, binding);
		next = SuccessorValue(value, underlying);
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
		NamedTypeInfo* info = model_.MutableInfo(type->named);
		if (info->is_defined)
			throw runtime_error("redefinition of enum " + name);
		info->is_defined = true;
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
