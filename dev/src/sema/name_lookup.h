#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "sema/entity.h"

// PA7 name lookup over the namespace model (3.4). Both lookups return
// a pointer into a Namespace's binding map, or null when nothing is
// found; they never modify the model, so the parser can also use them
// for speculative classification (the '(' disambiguation). Because the
// model is built by one forward pass, every binding and using-directive
// it contains was declared before the lookup point, which gives the
// declared-before-use rule for free.

enum ELookupFilter
{
	LF_ANY_ENTITY,
	// 3.4.6 / 3.4.3p1: lookup of a namespace-name (nested-name-specifier
	// components, using-directive and namespace-alias targets) ignores
	// non-namespace declarations instead of being blocked by them.
	LF_NAMESPACES_ONLY
};

// One using-directive's contribution to unqualified lookup: the
// nominated namespace's names appear as if declared in `anchor`
// (7.3.4p2).
struct ActiveDirective
{
	const Namespace* anchor;
	const Namespace* nominated;
};

// Memo of the transitive directive closure for one lexical scope chain
// (the chain is determined by its innermost namespace, so that pointer
// is the key). The holder must call Invalidate() whenever a
// using-directive is added anywhere in the model - explicit or implicit
// (unnamed/inline member creation) - because closures reach namespaces
// outside the current chain; entity and alias bindings do not affect
// the closure.
struct DirectiveClosureCache
{
	DirectiveClosureCache() : scope(0) {}

	void Invalidate()
	{
		scope = 0;
	}

	const Namespace* scope;  // null when invalid
	vector<ActiveDirective> directives;
};

// 3.4.1 with 7.3.4: walks the enclosing namespaces innermost-first;
// at each scope the visible declarations are its own plus those of
// every namespace nominated by an active using-directive (transitively
// closed) whose anchor - the nearest namespace enclosing both the
// directive and the nominated namespace - is that scope. `scopes` is
// the lexical chain, global first. `cache` (optional) memoizes the
// directive closure between consecutive lookups in the same scope.
const Binding* UnqualifiedLookup(const vector<Namespace*>& scopes,
                                 const string& name, ELookupFilter filter,
                                 DirectiveClosureCache* cache = 0);

// 3.4.3.2: searches `ns` and its inline namespace set; only if that
// finds nothing, recurses (each namespace at most once) into the
// namespaces nominated by using-directives in that set.
const Binding* QualifiedLookup(const Namespace& ns, const string& name,
                               ELookupFilter filter);
