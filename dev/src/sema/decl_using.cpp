#include "sema/decl_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;
using std::string;
using std::vector;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA11 assignment boundary");
}

}  // namespace

// Using-directives and using-declarations (7.3.3/7.3.4), split from
// decl_binder.cpp: namespace imports, member re-exposure, inherited
// constructors, and imported-overload merging.

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
	// PA24: a class-scope using-declaration re-exposing a base class's
	// conversion function is a no-op for this model - the conversion
	// set already resolves along the base chain.
	if (target.parts.back().kind == NP_CONVERSION_FUNCTION &&
	    current_->kind == SCOPE_CLASS)
	{
		Scope* named = ResolvePrefixScope(target);
		if (!named || named->kind != SCOPE_CLASS)
			throw runtime_error("using-declaration conversion target "
			                    "is not a class");
		return;
	}
	// TerminalName rejects template-ids, operators, and destructor
	// names (7.3.3p5 and the PA11 boundary).
	const string& name = TerminalName(target);
	Scope* prefix = ResolvePrefixScope(target);
	// 3.4.3.1p2: the terminal names the constructor when it repeats
	// the class name or the last nested-name-specifier component's
	// spelling (`using Base::Base`, `typedef Base A; using A::A`,
	// `using Base<T>::Base`).
	bool names_ctor = prefix->kind == SCOPE_CLASS &&
		(prefix->name == name ||
		 (target.parts.size() >= 2 &&
		  (target.parts[target.parts.size() - 2].kind == NP_IDENTIFIER ||
		   target.parts[target.parts.size() - 2].kind ==
		       NP_TEMPLATE_ID) &&
		  !target.parts[target.parts.size() - 2].tilde &&
		  target.parts[target.parts.size() - 2].identifier == name));
	if (names_ctor)
	{
		// 12.9: `using Base::Base` inherits the base constructors.
		BindInheritingConstructors(prefix);
		return;
	}
	const ScopeBinding* found = QualifiedLookup(*prefix, name, SLF_ANY);
	if (!found && decl.using_if_exists)
		return;  // Clang using_if_exists: a missing target binds nothing
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
		// 7.3.3p15 (classes) and 7.3.3p11 (namespaces): the scope's
		// own function declarations and the imported overloads form
		// one set; repeated imports merge idempotently (hosted
		// <cstdlib> re-imports ::div beside std's own overloads).
		// 7.3.3p10/3.3.10: importing a type name over an existing alias
		// for the same type is a harmless redeclaration (the hosted
		// stdlib.h wrapper's `using std::div_t;` beside glibc's own
		// typedef).
		if ((existing->kind == SB_TYPE ||
		     existing->kind == SB_TYPE_ALIAS) &&
		    (imported.kind == SB_TYPE ||
		     imported.kind == SB_TYPE_ALIAS) &&
		    existing->type && imported.type &&
		    TypeEquals(existing->type, imported.type))
			return;
		// 7.3.3p10 likewise for templates: repeated imports of the
		// same template entity are harmless redeclarations (two
		// headers each spelling `using std::vector;`).
		if (existing->kind == imported.kind && existing->templ &&
		    existing->templ == imported.templ)
			return;
		if ((current_->kind != SCOPE_CLASS &&
		     current_->kind != SCOPE_NAMESPACE) ||
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
	// 7.3.3p15: imported member templates join the set unless an own
	// template with the same signature hides them.
	for (size_t i = 0; i < imported.fn_templates.size(); i++)
	{
		bool hidden = false;
		for (size_t j = 0; !hidden && j < own.fn_templates.size(); j++)
			hidden = SameImportedTemplateSignature(
				*own.fn_templates[j], *imported.fn_templates[i]);
		if (!hidden)
			own.fn_templates.push_back(imported.fn_templates[i]);
	}
	vector<TypePtr> incoming;
	if (imported.type)
	{
		incoming.push_back(imported.type);
		for (size_t i = 0; i < imported.overloads.size(); i++)
			incoming.push_back(imported.overloads[i]);
	}
	for (size_t i = 0; i < incoming.size(); i++)
	{
		bool replaced = own.type &&
			TypeEquals(RemoveTopCv(own.type), RemoveTopCv(incoming[i]));
		for (size_t j = 0; !replaced && j < own.overloads.size(); j++)
			if (TypeEquals(RemoveTopCv(own.overloads[j]),
			               RemoveTopCv(incoming[i])))
				replaced = true;
		if (replaced)
			continue;
		size_t at;
		if (!own.type)
		{
			// A template-only set gains its first ordinary entry.
			own.type = incoming[i];
			at = 0;
		}
		else
		{
			own.overloads.push_back(incoming[i]);
			at = own.overloads.size();
		}
		size_t count = own.overloads.size() + 1;
		own.fn_defaults.resize(count);
		own.fn_deleted.resize(count, false);
		own.fn_access.resize(count, MA_PUBLIC);
		own.fn_static.resize(count, false);
		own.fn_inline_def.resize(count, false);
		own.fn_adl_only.resize(count, false);
		own.fn_unwind_no.resize(count, false);
		own.fn_noexcept_decl.resize(count, false);
		own.fn_noexcept_pending.resize(count);
		own.fn_owner.resize(count, 0);
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
		if (i < imported.fn_noexcept_decl.size())
			own.fn_noexcept_decl[at] = imported.fn_noexcept_decl[i];
		if (i < imported.fn_noexcept_pending.size())
			own.fn_noexcept_pending[at] = imported.fn_noexcept_pending[i];
	}
}
