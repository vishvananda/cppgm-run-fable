#include "sema/decl_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA11 function-definition binding, split from decl_binder.cpp: the
// declarator composes in the named scope (member signatures see the
// class), the (possibly qualified) name binds, and the body binds in
// the new function scope.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA11 assignment boundary");
}

}  // namespace

void DeclBinder::BindFunctionDefinition(const AstDecl& decl)
{
	if (HasFriendSpecifier(decl))
	{
		BindFriendDeclaration(decl);
		return;
	}
	// 7.5p7 applies to the definition itself, not to declarations
	// nested inside its body.
	in_linkage_single_ = false;
	size_t collapse_before = collapsed_alias_uses_;
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (specs.is_typedef)
		throw runtime_error("typedef on a function definition");
	// 3.4.3p3/8.3.5: names in the declarator after a qualified
	// declarator-id (parameters, trailing return type) resolve in the
	// named class's scope.
	Scope* compose_scope = current_;
	const AstName* declared_id =
		decl.declarator ? decl.declarator->IdName() : 0;
	if (declared_id &&
	    (declared_id->parts.size() > 1 || declared_id->global_scope))
	{
		Scope* prefix = ResolvePrefixScope(*declared_id);
		if (prefix && prefix->kind == SCOPE_CLASS)
			compose_scope = prefix;
	}
	Scope* saved_compose = current_;
	current_ = compose_scope;
	bool member_signature = compose_scope->kind == SCOPE_CLASS;
	if (member_signature)
		OnMemberSignatureBegin(compose_scope);
	DeclaratorInfo composed;
	try
	{
		composed = builder_.ComposeDeclarator(decl.declarator.get(),
		                                      specs.type);
	}
	catch (...)
	{
		if (member_signature)
			OnMemberSignatureEnd();
		current_ = saved_compose;
		throw;
	}
	if (member_signature)
		OnMemberSignatureEnd();
	current_ = saved_compose;
	composed_alias_collapsed_ = collapsed_alias_uses_ != collapse_before;
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
	scope->fn_type = composed.type;
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

