#pragma once

#include <string>

using std::string;

#include "sema/scope.h"

// PA11 name lookup over the scope model (3.4). Results point into the
// model's bindings; because the model is built by one forward pass,
// everything a lookup can find was declared before the lookup point.
// Both lookups throw on ambiguity (distinct bindings reached through
// different using-directive paths, 3.4.1p2 / 3.4.3.2p3) and return
// null when nothing is found.

enum EScopeLookupFilter
{
	SLF_ANY,
	// 3.4.3p1 / 3.4.6: lookup of a nested-name-specifier component,
	// using-directive target, or namespace-alias target considers only
	// names that can introduce a scope (namespaces, their aliases,
	// types, and type aliases).
	SLF_SCOPE_NAMES
};

// 3.4.1 with 7.3.4: walks the lexical scope chain innermost-first; at
// each scope the visible declarations are its own plus the members of
// every namespace nominated by an active using-directive (transitively
// closed) whose anchor - the nearest namespace enclosing both the
// directive and the nominated namespace - is that scope. A scope's own
// binding wins over directive-contributed ones.
const ScopeBinding* UnqualifiedLookup(const Scope* from, const string& name,
                                      EScopeLookupFilter filter);

// 3.4.3.2 for namespace scopes: searches `scope` itself, then (only if
// that finds nothing) the namespaces nominated by its using-directives,
// each namespace at most once. Class and enum scopes search their own
// bindings only.
const ScopeBinding* QualifiedLookup(const Scope& scope, const string& name,
                                    EScopeLookupFilter filter);
