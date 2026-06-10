#include "sema/entity.h"

#include <algorithm>

using std::find;

SemaModel::SemaModel()
{
	namespaces_.emplace_back(new Namespace());
	global_ = namespaces_.back().get();
	global_->is_unnamed = true;
}

Namespace* SemaModel::CreateNamespace(const string& name, bool is_unnamed,
                                      bool is_inline, Namespace* parent)
{
	namespaces_.emplace_back(new Namespace());
	Namespace* ns = namespaces_.back().get();
	ns->name = name;
	ns->is_unnamed = is_unnamed;
	ns->is_inline = is_inline;
	ns->parent = parent;
	return ns;
}

DeclaredEntity* SemaModel::CreateEntity(const string& name,
                                        const TypePtr& type)
{
	entities_.emplace_back(new DeclaredEntity());
	DeclaredEntity* entity = entities_.back().get();
	entity->name = name;
	entity->type = type;
	return entity;
}

Namespace* AddMemberNamespace(SemaModel& model, Namespace& parent,
                              const string& name, bool is_unnamed,
                              bool is_inline)
{
	Namespace* ns = model.CreateNamespace(is_unnamed ? string() : name,
	                                      is_unnamed, is_inline, &parent);
	parent.members.push_back(ns);
	if (is_unnamed)
		parent.unnamed_member = ns;
	else
	{
		Binding binding;
		binding.kind = BK_NAMESPACE;
		binding.target = ns;
		parent.bindings[name] = binding;
	}
	if (is_unnamed || is_inline)
		AddUsingDirective(parent, ns);
	return ns;
}

void AddUsingDirective(Namespace& ns, Namespace* nominated)
{
	if (find(ns.using_directives.begin(), ns.using_directives.end(),
	         nominated) == ns.using_directives.end())
		ns.using_directives.push_back(nominated);
}

void DescribeNamespace(ostream& out, const Namespace& ns)
{
	if (ns.name.empty())
		out << "start unnamed namespace\n";
	else
		out << "start namespace " << ns.name << "\n";
	if (ns.is_inline)
		out << "inline namespace\n";
	for (size_t i = 0; i < ns.variables.size(); i++)
		out << "variable " << ns.variables[i]->name << " "
		    << DescribeType(ns.variables[i]->type) << "\n";
	for (size_t i = 0; i < ns.functions.size(); i++)
		out << "function " << ns.functions[i]->name << " "
		    << DescribeType(ns.functions[i]->type) << "\n";
	for (size_t i = 0; i < ns.members.size(); i++)
		DescribeNamespace(out, *ns.members[i]);
	out << "end namespace\n";
}
