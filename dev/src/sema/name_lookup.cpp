#include "sema/name_lookup.h"

#include <set>
#include <stdexcept>
#include <utility>

using std::make_pair;
using std::pair;
using std::runtime_error;
using std::set;

namespace {

const Binding* MatchInNamespace(const Namespace& ns, const string& name,
                                ELookupFilter filter)
{
	map<string, Binding>::const_iterator it = ns.bindings.find(name);
	if (it == ns.bindings.end())
		return 0;
	if (filter == LF_NAMESPACES_ONLY && it->second.kind != BK_NAMESPACE)
		return 0;
	return &it->second;
}

// Merges one found declaration into the accumulated result. Function
// sets union (each entity once); any other combination of distinct
// entities is an ambiguous lookup (3.4.1p2), a diagnosable error.
void MergeCandidate(bool& found, Binding& out, const Binding& candidate,
                    const string& name)
{
	if (!found)
	{
		found = true;
		out = candidate;
		return;
	}
	if (out.kind == BK_FUNCTION && candidate.kind == BK_FUNCTION)
	{
		for (size_t i = 0; i < candidate.overloads.size(); i++)
		{
			bool present = false;
			for (size_t j = 0; j < out.overloads.size(); j++)
				if (out.overloads[j].entity == candidate.overloads[i].entity)
					present = true;
			if (!present)
				out.overloads.push_back(candidate.overloads[i]);
		}
		return;
	}
	bool same = false;
	if (out.kind == candidate.kind)
	{
		switch (out.kind)
		{
		case BK_VARIABLE:
			same = out.entity == candidate.entity;
			break;
		case BK_NAMESPACE:
			same = out.target == candidate.target;
			break;
		case BK_TYPEDEF:
			same = TypeEquals(out.type, candidate.type);
			break;
		case BK_FUNCTION:
			break;
		}
	}
	if (!same)
		throw runtime_error("lookup of `" + name + "` is ambiguous");
}

// True when `inner` is declared inside `outer` at any depth.
bool NamespaceContains(const Namespace* outer, const Namespace* inner)
{
	for (const Namespace* walk = inner->parent; walk; walk = walk->parent)
		if (walk == outer)
			return true;
	return false;
}

// 7.3.4p2: the namespace whose lookup the directive contributes to is
// the nearest one enclosing both the directive (in `container`) and the
// nominated namespace.
const Namespace* DirectiveAnchor(const Namespace* container,
                                 const Namespace* nominated)
{
	const Namespace* walk = container;
	while (walk->parent && !NamespaceContains(walk, nominated))
		walk = walk->parent;
	return walk;
}

typedef set<pair<const Namespace*, const Namespace*>> DirectiveSeen;

// 7.3.4p4: directives are transitive for unqualified lookup - a
// directive found in a nominated namespace acts as if it appeared at
// the original directive's container, so `container` stays fixed
// through the recursion.
void AddActiveDirectives(const Namespace* container,
                         const Namespace* nominated, DirectiveSeen& seen,
                         vector<ActiveDirective>& out)
{
	if (!seen.insert(make_pair(container, nominated)).second)
		return;
	ActiveDirective directive;
	directive.anchor = DirectiveAnchor(container, nominated);
	directive.nominated = nominated;
	out.push_back(directive);
	for (size_t i = 0; i < nominated->using_directives.size(); i++)
		AddActiveDirectives(container, nominated->using_directives[i], seen,
		                    out);
}

// The inline namespace set of `ns` (7.3.1p9): itself plus, transitively,
// its inline member namespaces.
void CollectInlineSet(const Namespace& ns, vector<const Namespace*>& out)
{
	out.push_back(&ns);
	for (size_t i = 0; i < ns.members.size(); i++)
		if (ns.members[i]->is_inline)
			CollectInlineSet(*ns.members[i], out);
}

bool QualifiedLookupRec(const Namespace& ns, const string& name,
                        ELookupFilter filter, Binding& out,
                        set<const Namespace*>& visited)
{
	if (!visited.insert(&ns).second)
		return false;
	vector<const Namespace*> inline_set;
	CollectInlineSet(ns, inline_set);
	bool found = false;
	for (size_t i = 0; i < inline_set.size(); i++)
	{
		const Binding* match = MatchInNamespace(*inline_set[i], name, filter);
		if (match)
			MergeCandidate(found, out, *match, name);
	}
	if (found)
		return true;
	for (size_t i = 0; i < inline_set.size(); i++)
	{
		const vector<Namespace*>& directives = inline_set[i]->using_directives;
		for (size_t j = 0; j < directives.size(); j++)
		{
			Binding sub;
			if (QualifiedLookupRec(*directives[j], name, filter, sub, visited))
				MergeCandidate(found, out, sub, name);
		}
	}
	return found;
}

} // namespace

bool UnqualifiedLookup(const vector<Namespace*>& scopes, const string& name,
                       ELookupFilter filter, Binding& out,
                       DirectiveClosureCache* cache)
{
	vector<ActiveDirective> local;
	const vector<ActiveDirective>* directives = &local;
	if (cache && cache->scope == scopes.back())
		directives = &cache->directives;
	else
	{
		DirectiveSeen seen;
		for (size_t i = 0; i < scopes.size(); i++)
			for (size_t j = 0; j < scopes[i]->using_directives.size(); j++)
				AddActiveDirectives(scopes[i], scopes[i]->using_directives[j],
				                    seen, local);
		if (cache)
		{
			cache->directives.swap(local);
			cache->scope = scopes.back();
			directives = &cache->directives;
		}
	}
	for (size_t i = scopes.size(); i-- > 0;)
	{
		bool found = false;
		const Binding* own = MatchInNamespace(*scopes[i], name, filter);
		if (own)
			MergeCandidate(found, out, *own, name);
		for (size_t j = 0; j < directives->size(); j++)
		{
			if ((*directives)[j].anchor != scopes[i])
				continue;
			const Binding* match =
				MatchInNamespace(*(*directives)[j].nominated, name, filter);
			if (match)
				MergeCandidate(found, out, *match, name);
		}
		if (found)
			return true;
	}
	return false;
}

bool QualifiedLookup(const Namespace& ns, const string& name,
                     ELookupFilter filter, Binding& out)
{
	set<const Namespace*> visited;
	return QualifiedLookupRec(ns, name, filter, out, visited);
}

const Binding* MemberLookup(const Namespace& ns, const string& name)
{
	vector<const Namespace*> inline_set;
	CollectInlineSet(ns, inline_set);
	for (size_t i = 0; i < inline_set.size(); i++)
	{
		map<string, Binding>::const_iterator it =
			inline_set[i]->bindings.find(name);
		if (it != inline_set[i]->bindings.end())
			return &it->second;
	}
	return 0;
}
