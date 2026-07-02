#include "sema/scope_lookup.h"

#include <set>
#include <stdexcept>
#include <vector>

using std::runtime_error;
using std::set;
using std::vector;

namespace {

// 7.3.1.2p3: a function declared only by friend declarations is
// invisible to ordinary lookup (argument-dependent lookup still
// considers it).
bool HiddenFriendOnly(const ScopeBinding& binding)
{
	if (binding.kind != SB_FUNCTION || binding.fn_adl_only.empty())
		return false;
	size_t count = binding.overloads.size() + 1;
	for (size_t i = 0; i < count; i++)
		if (i >= binding.fn_adl_only.size() || !binding.fn_adl_only[i])
			return false;
	return true;
}

bool BindingPassesFilter(const ScopeBinding& binding,
                         EScopeLookupFilter filter)
{
	if (HiddenFriendOnly(binding))
		return false;
	if (filter == SLF_ANY)
		return true;
	switch (binding.kind)
	{
	case SB_NAMESPACE:
	case SB_NAMESPACE_ALIAS:
	case SB_TYPE:
	case SB_TYPE_ALIAS:
		return true;
	default:
		return false;
	}
}

// 10.2 single-inheritance member lookup: the class's own declarations
// hide base declarations; otherwise the search continues up the base
// chain.
const ScopeBinding* ClassChainLookup(const Scope& scope, const string& name,
                                     EScopeLookupFilter filter,
                                     bool skip_dependent_base = false)
{
	for (const Scope* link = &scope; link; link = link->class_base)
	{
		const ScopeBinding* own = FindOwnBinding(*link, name);
		if (own && BindingPassesFilter(*own, filter))
			return own;
		// PA18 14.6.2p3: an unqualified name does not search a base
		// that was dependent in the template pattern.
		if (skip_dependent_base && link->base_dependent)
			break;
	}
	return 0;
}

const Scope* EnclosingNamespace(const Scope* scope)
{
	while (scope && scope->kind != SCOPE_NAMESPACE)
		scope = scope->parent;
	return scope;
}

// 7.3.4p2: the namespace whose declarations a directive's names join -
// the nearest namespace enclosing both the directive's lexical scope
// and the nominated namespace.
const Scope* DirectiveAnchor(const Scope* site, const Scope* nominated)
{
	set<const Scope*> site_namespaces;
	for (const Scope* scope = EnclosingNamespace(site); scope;
	     scope = EnclosingNamespace(scope->parent))
		site_namespaces.insert(scope);
	for (const Scope* scope = EnclosingNamespace(nominated->parent); scope;
	     scope = EnclosingNamespace(scope->parent))
		if (site_namespaces.count(scope))
			return scope;
	return 0;
}

struct ActiveDirective
{
	const Scope* anchor;
	const Scope* nominated;
};

// The transitive closure of the using-directives visible from the
// lexical chain of `from` (7.3.4p4: directives inside a nominated
// namespace are active too, anchored at their own location).
vector<ActiveDirective> DirectiveClosure(const Scope* from)
{
	vector<ActiveDirective> closure;
	set<const Scope*> nominated_seen;
	vector<const Scope*> worklist;
	for (const Scope* scope = from; scope; scope = scope->parent)
		worklist.push_back(scope);
	// Each worklist entry is a scope whose lexical directives are
	// active; nominated namespaces join the list once each.
	for (size_t i = 0; i < worklist.size(); i++)
	{
		const Scope* site = worklist[i];
		for (size_t j = 0; j < site->using_directives.size(); j++)
		{
			const Scope* nominated = site->using_directives[j];
			ActiveDirective directive;
			directive.anchor = DirectiveAnchor(site, nominated);
			directive.nominated = nominated;
			closure.push_back(directive);
			if (nominated_seen.insert(nominated).second)
				worklist.push_back(nominated);
		}
	}
	return closure;
}

// 7.3.4p6 / 3.4.3.2p3: declarations found in several namespaces are
// ambiguous only when they do not declare the same entity - a
// namespace seen beside its aliases, the same type reached through
// different names, or one declaration imported into several scopes by
// using-declarations (imports share the entity's type node and value).
bool SameFoundEntity(const ScopeBinding& a, const ScopeBinding& b)
{
	bool a_namespace = a.kind == SB_NAMESPACE || a.kind == SB_NAMESPACE_ALIAS;
	bool b_namespace = b.kind == SB_NAMESPACE || b.kind == SB_NAMESPACE_ALIAS;
	if (a_namespace || b_namespace)
		return a_namespace && b_namespace && a.target == b.target;
	bool a_type = a.kind == SB_TYPE || a.kind == SB_TYPE_ALIAS;
	bool b_type = b.kind == SB_TYPE || b.kind == SB_TYPE_ALIAS;
	if (a_type || b_type)
		return a_type && b_type && TypeEquals(a.type, b.type);
	// PA18: class-template names found along several paths name the
	// same entity when they share the template record.
	if (a.kind == SB_CLASS_TEMPLATE || b.kind == SB_CLASS_TEMPLATE)
		return a.kind == b.kind && a.templ == b.templ;
	return a.kind == b.kind && a.type == b.type &&
		a.has_value == b.has_value && a.value.bits == b.value.bits;
}

// Merges one more found declaration into the result; bindings naming
// distinct entities make the lookup ambiguous. Function bindings
// collect apart: same-level function declarations form one overload
// set instead of an ambiguity (7.3.4p6).
void MergeFound(const ScopeBinding* candidate, const ScopeBinding*& result,
                vector<const ScopeBinding*>& functions)
{
	if (candidate->kind == SB_FUNCTION)
	{
		// The same declaration reached through several import paths
		// shares its type node (using-declarations copy the binding).
		for (size_t i = 0; i < functions.size(); i++)
			if (functions[i] == candidate ||
			    functions[i]->type == candidate->type)
				return;
		functions.push_back(candidate);
		return;
	}
	if (!result)
		result = candidate;
	else if (result != candidate && !SameFoundEntity(*result, *candidate))
		throw AmbiguousLookupError("ambiguous name lookup of " +
		                           candidate->name);
}

// Defined below (3.4.3.2p2 qualified search); the directive walk
// reuses it so names surfaced into a nominated namespace by its own
// directives (notably inline member namespaces, 7.3.1p9/7.3.4p4) are
// visible through an outer directive as well.
const ScopeBinding* QualifiedNamespaceSearch(const Scope& scope,
                                             const string& name,
                                             EScopeLookupFilter filter,
                                             set<const Scope*>& visited);

}  // namespace

const ScopeBinding* UnqualifiedLookup(const Scope* from, const string& name,
                                      EScopeLookupFilter filter,
                                      vector<const ScopeBinding*>* fn_set)
{
	vector<ActiveDirective> closure = DirectiveClosure(from);
	for (const Scope* scope = from; scope; scope = scope->parent)
	{
		const ScopeBinding* own = scope->kind == SCOPE_CLASS
			? ClassChainLookup(*scope, name, filter, true)
			: FindOwnBinding(*scope, name);
		if (own && !BindingPassesFilter(*own, filter))
			own = 0;
		if (scope->kind != SCOPE_NAMESPACE)
		{
			if (own)
				return own;
			continue;
		}
		// 7.3.4p2/p6: directive-imported names appear beside the
		// namespace's own declarations; distinct non-function entities
		// found at the same level make the name ambiguous.
		const ScopeBinding* found = 0;
		vector<const ScopeBinding*> functions;
		if (own)
			MergeFound(own, found, functions);
		for (size_t i = 0; i < closure.size(); i++)
		{
			if (closure[i].anchor != scope)
				continue;
			set<const Scope*> visited;
			const ScopeBinding* member = QualifiedNamespaceSearch(
				*closure[i].nominated, name, filter, visited);
			if (member)
				MergeFound(member, found, functions);
		}
		if (found && !functions.empty())
			throw AmbiguousLookupError("ambiguous name lookup of " +
			                           name);
		if (!functions.empty())
		{
			if (functions.size() > 1 && !fn_set)
				throw AmbiguousLookupError("ambiguous name lookup of " +
				                           name);
			if (fn_set)
				*fn_set = functions;
			return functions[0];
		}
		if (found)
			return found;
	}
	return 0;
}

namespace {

// 3.4.3.2p2: the declarations of `name` directly in `scope`, or - only
// when there are none - the union over the namespaces nominated by its
// using-directives, each namespace searched at most once.
const ScopeBinding* QualifiedNamespaceSearch(const Scope& scope,
                                             const string& name,
                                             EScopeLookupFilter filter,
                                             set<const Scope*>& visited)
{
	if (!visited.insert(&scope).second)
		return 0;
	const ScopeBinding* own = FindOwnBinding(scope, name);
	if (own && BindingPassesFilter(*own, filter))
		return own;
	const ScopeBinding* found = 0;
	vector<const ScopeBinding*> functions;
	for (size_t i = 0; i < scope.using_directives.size(); i++)
	{
		const ScopeBinding* member = QualifiedNamespaceSearch(
			*scope.using_directives[i], name, filter, visited);
		if (member)
			MergeFound(member, found, functions);
	}
	// Qualified lookup resolves to one binding: distinct functions
	// reached through several nominated namespaces stay ambiguous
	// here (no caller resolves a merged set through this path).
	if (functions.size() > 1 || (found && !functions.empty()))
		throw AmbiguousLookupError("ambiguous name lookup of " + name);
	return functions.empty() ? found : functions[0];
}

}  // namespace

const ScopeBinding* QualifiedLookup(const Scope& scope, const string& name,
                                    EScopeLookupFilter filter)
{
	if (scope.kind == SCOPE_CLASS)
		return ClassChainLookup(scope, name, filter);
	if (scope.kind != SCOPE_NAMESPACE)
	{
		const ScopeBinding* own = FindOwnBinding(scope, name);
		return own && BindingPassesFilter(*own, filter) ? own : 0;
	}
	set<const Scope*> visited;
	return QualifiedNamespaceSearch(scope, name, filter, visited);
}
