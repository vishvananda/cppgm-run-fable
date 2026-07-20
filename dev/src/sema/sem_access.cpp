#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA17/PA26 11 member access control: the member-access check over the
// current method/class contexts (including 11.2 out-of-class member
// definitions and 11.2p5/11.4 protected-through-path grants) and the
// per-edge base-path check. Split from sem_class.cpp, which keeps the
// class recording and layout machinery.

void SemBinder::CheckMemberAccess(const Scope* owner, EMemberAccess access,
                                  const string& what,
                                  const NamedTypeInfo* naming)
{
	const NamedTypeInfo* owner_entity = model_.ScopeEntity(owner);
	if (access == MA_PUBLIC &&
	    (!naming || naming == owner_entity))
		return;
	const ClassInfo* owner_cls =
		owner_entity ? unit_.classes.Find(owner_entity) : 0;
	if (!owner_cls)
		return;
	vector<const ClassInfo*> contexts;
	if (method_.cls)
		contexts.push_back(method_.cls);
	if (method_.lexical_cls)
		contexts.push_back(method_.lexical_cls);
	// 11.2: an out-of-class member definition checks access as a
	// member of its class (its return type resolves before the class
	// scope is entered).
	for (size_t i = 0; i < access_class_contexts_.size(); i++)
		if (access_class_contexts_[i])
			contexts.push_back(access_class_contexts_[i]);
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS)
			if (const NamedTypeInfo* entity = model_.ScopeEntity(scope))
				if (const ClassInfo* cls = unit_.classes.Find(entity))
					contexts.push_back(cls);
	// 11.2p1/p4: each non-public base edge on the naming class's
	// derivation path to the owner restricts access further - a private
	// base's members are reachable only inside the deriving class (or
	// its friends), a protected base's also inside classes derived from
	// it.
	if (naming && owner_entity && naming != owner_entity)
	{
		vector<ClassBaseEdge> edges;
		if (BaseAccessPath(naming, owner_entity, edges))
			for (size_t e = 0; e < edges.size(); e++)
				CheckBaseEdgeAccess(edges[e], contexts, what);
	}
	if (access == MA_PUBLIC)
		return;
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i]->members == owner)
			return;
		if (access == MA_PROTECTED)
		{
			// The context may derive from the owner through any path
			// of the base DAG (PA26).
			vector<const ClassInfo*> chain;
			CollectClassAndBases(contexts[i], chain);
			for (size_t j = 0; j < chain.size(); j++)
				if (chain[j]->members == owner)
					return;
		}
		// A friend class's members access everything (11.3); a friend
		// class template's grant covers every specialization.
		for (size_t j = 0; j < owner_cls->friend_classes.size(); j++)
			if (FriendClassMatches(owner_cls->friend_classes[j],
			                       contexts[i]->entity))
				return;
	}
	// A friend captured inside an enclosing instantiation declares
	// through the specialization's alias scope; friendship recorded
	// its home namespace.
	const Scope* fn_home = method_.fn_owner;
	while (fn_home && fn_home->kind == SCOPE_TEMPLATE_PARAMS)
		fn_home = fn_home->parent;
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < owner_cls->friend_functions.size(); i++)
			if (owner_cls->friend_functions[i].first == fn_home &&
			    (owner_cls->friend_functions[i].second ==
			         method_.fn_name ||
			     (!method_.fn_template_name.empty() &&
			      owner_cls->friend_functions[i].second ==
			          method_.fn_template_name)))
				return;
	// 11.2p5/11.4: a protected member of a base class is also
	// accessible to members and friends of any class P on the object
	// expression's derivation path to the owner; the naming class is
	// derived from P by construction, satisfying the 11.4 object-type
	// restriction. The path may run through any branch of the base DAG
	// (PA26).
	if (access == MA_PROTECTED && naming)
	{
		vector<const ClassInfo*> path;
		CollectClassAndBases(unit_.classes.Find(naming), path);
		for (size_t k = 0; k < path.size(); k++)
		{
			const ClassInfo* p = path[k];
			if (p->members == owner ||
			    !DerivedFromWithExtrasLinked(p->entity, owner_entity))
				continue;
			for (size_t i = 0; i < contexts.size(); i++)
				for (size_t j = 0; j < p->friend_classes.size(); j++)
					if (FriendClassMatches(p->friend_classes[j],
					                       contexts[i]->entity))
						return;
			if (!method_.fn_name.empty())
				for (size_t i = 0; i < p->friend_functions.size(); i++)
					if (p->friend_functions[i].first == fn_home &&
					    p->friend_functions[i].second ==
					        method_.fn_name)
						return;
		}
	}
	throw runtime_error(what + " is inaccessible in this context");
}

// One base-specifier edge of the naming class's derivation path: a
// non-public edge admits the deriving class itself, its friends, and
// (protected) classes derived from it (11.2p1/p4).
void SemBinder::CheckBaseEdgeAccess(const ClassBaseEdge& edge,
                                    const vector<const ClassInfo*>& contexts,
                                    const string& what)
{
	EMemberAccess edge_access =
		edge.derived->direct_bases[edge.base_index].access;
	if (edge_access == MA_PUBLIC)
		return;
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i] == edge.derived)
			return;
		if (edge_access == MA_PROTECTED &&
		    DerivedFromWithExtrasLinked(contexts[i]->entity,
		                                edge.derived->entity))
			return;
		for (size_t j = 0; j < edge.derived->friend_classes.size(); j++)
			if (FriendClassMatches(edge.derived->friend_classes[j],
			                       contexts[i]->entity))
				return;
	}
	const Scope* fn_home = method_.fn_owner;
	while (fn_home && fn_home->kind == SCOPE_TEMPLATE_PARAMS)
		fn_home = fn_home->parent;
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < edge.derived->friend_functions.size(); i++)
			if (edge.derived->friend_functions[i].first == fn_home &&
			    edge.derived->friend_functions[i].second == method_.fn_name)
				return;
	throw runtime_error(what + " is inaccessible in this context");
}

