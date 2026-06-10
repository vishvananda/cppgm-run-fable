#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "sema/entity.h"

// PA7/PA8 name lookup over the namespace model (3.4). Lookups return
// the result by value because declarations reached through different
// paths merge: function declarations found in multiple namespaces form
// one overload set, while distinct non-function entities make the
// lookup ambiguous (3.4.1p2, 3.4.3.2p3) - a diagnosable error in PA8.
// Lookups never modify the model, so the parser can also use them for
// speculative classification (the '(' disambiguation). Because the
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
// directive and the nominated namespace - is that scope. All
// declarations visible at the innermost scope that has any are
// merged into `out`; distinct non-function entities throw (ambiguous).
// `scopes` is the lexical chain, global first. `cache` (optional)
// memoizes the directive closure between consecutive lookups in the
// same scope. Returns false when nothing is found.
bool UnqualifiedLookup(const vector<Namespace*>& scopes, const string& name,
                       ELookupFilter filter, Binding& out,
                       DirectiveClosureCache* cache = 0);

// 3.4.3.2: searches `ns` and its inline namespace set; only if that
// finds nothing, recurses (each namespace at most once) into the
// namespaces nominated by using-directives in that set. Results merge
// like UnqualifiedLookup.
bool QualifiedLookup(const Namespace& ns, const string& name,
                     ELookupFilter filter, Binding& out);

// 7.3.1.2p2 / 3.4.3p3 support: the binding of a member declared in
// `ns` or its inline namespace set (no using-directive traversal),
// for redeclaration through a qualified declarator-id. Imported
// bindings are returned as found; the caller rejects them.
const Binding* MemberLookup(const Namespace& ns, const string& name);
