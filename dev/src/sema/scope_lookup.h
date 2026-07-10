#pragma once

#include <stdexcept>
#include <string>

using std::string;

#include "sema/scope.h"

// A lookup that reached distinct entities through using-directives
// (3.4.1p2 / 7.3.4p6): a diagnosable error that call recovery paths
// (builtin fallback, ADL retry) must not mask as name-not-found.
struct AmbiguousLookupError : std::runtime_error
{
	explicit AmbiguousLookupError(const string& what)
		: std::runtime_error(what)
	{}
};

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
//
// 7.3.4p6: several same-level *function* bindings are not ambiguous -
// their declarations form one overload set. With `fn_set` given the
// set's bindings are collected there (own binding first) and the first
// is returned; without it, a multi-binding function result throws so
// callers that cannot resolve overload sets never pick one silently.
const ScopeBinding* UnqualifiedLookup(const Scope* from, const string& name,
                                      EScopeLookupFilter filter,
                                      vector<const ScopeBinding*>* fn_set = 0);

// 3.4.3.2 for namespace scopes: searches `scope` itself, then (only if
// that finds nothing) the namespaces nominated by its using-directives,
// each namespace at most once. Class and enum scopes search their own
// bindings only.
const ScopeBinding* QualifiedLookup(const Scope& scope, const string& name,
                                    EScopeLookupFilter filter);

// PA26 10.2: member lookup of `name` in a class member scope and its
// non-virtual base DAG (own declarations hide base ones). Throws
// AmbiguousLookupError when distinct declarations are reachable in
// sibling base subobjects.
const ScopeBinding* ClassMemberLookup(const Scope& scope, const string& name);
