#include "sema/scope.h"

#include <stdexcept>

using std::runtime_error;
using std::to_string;

TypesModel::TypesModel()
{
	global_ = CreateScope(SCOPE_NAMESPACE, "", 0);
}

Scope* TypesModel::CreateScope(EScopeKind kind, const string& name,
                               Scope* parent)
{
	scopes_.push_back(unique_ptr<Scope>(new Scope()));
	Scope* scope = scopes_.back().get();
	scope->kind = kind;
	scope->name = name;
	scope->parent = parent;
	if (parent)
		parent->children.push_back(scope);
	return scope;
}

NamedTypeInfo* TypesModel::CreateNamedTypeInfo(const string& display,
                                               const Scope* scope,
                                               const string& name)
{
	infos_.push_back(unique_ptr<NamedTypeInfo>(new NamedTypeInfo()));
	infos_.back()->display = display;
	infos_.back()->scope = scope;
	infos_.back()->name = name;
	return infos_.back().get();
}

NamedTypeInfo* TypesModel::MutableInfo(const NamedTypeInfo* info)
{
	return const_cast<NamedTypeInfo*>(info);
}

void TypesModel::SetMemberScope(const NamedTypeInfo* info, Scope* scope)
{
	scope->entity = info;
	member_scopes_[info] = scope;
}

const NamedTypeInfo* TypesModel::ScopeEntity(const Scope* scope) const
{
	return scope ? scope->entity : 0;
}

Scope* TypesModel::MemberScope(const NamedTypeInfo* info) const
{
	map<const NamedTypeInfo*, Scope*>::const_iterator found =
		member_scopes_.find(info);
	return found == member_scopes_.end() ? 0 : found->second;
}

ScopeBinding* FindOwnBinding(Scope& scope, const string& name)
{
	map<string, size_t>::const_iterator found =
		scope.binding_index.find(name);
	return found == scope.binding_index.end()
		? 0 : &scope.bindings[found->second];
}

const ScopeBinding* FindOwnBinding(const Scope& scope, const string& name)
{
	return FindOwnBinding(const_cast<Scope&>(scope), name);
}

// Monotonic declaration order across the whole bind; binding runs
// sequentially, so a plain counter suffices.
static size_t binding_seq_counter = 1;

size_t CurrentBindingSeq()
{
	return binding_seq_counter;
}

ScopeBinding& AddBinding(Scope& scope, const ScopeBinding& binding)
{
	if (scope.binding_index.count(binding.name))
		throw runtime_error("redeclaration of " + binding.name);
	// 3.3.3p2: a parameter name shall not be redeclared in the
	// outermost block of its function definition (function scopes hold
	// only parameter bindings).
	if (scope.kind == SCOPE_BLOCK && scope.parent &&
	    scope.parent->kind == SCOPE_FUNCTION &&
	    FindOwnBinding(*scope.parent, binding.name))
		throw runtime_error(binding.name + " redeclares a parameter");
	scope.binding_index[binding.name] = scope.bindings.size();
	scope.bindings.push_back(binding);
	scope.bindings.back().seq = binding_seq_counter++;
	if (!scope.bindings.back().owner)
		scope.bindings.back().owner = &scope;
	scope.bindings.back().home = &scope;
	return scope.bindings.back();
}

void AddUsingDirective(Scope& scope, Scope* nominated)
{
	for (size_t i = 0; i < scope.using_directives.size(); i++)
		if (scope.using_directives[i] == nominated)
			return;
	scope.using_directives.push_back(nominated);
}

string RenderConstValue(const ConstValue& value)
{
	if (IsSignedIntegralFundamental(value.type))
		return to_string((long long)value.bits);
	return to_string(value.bits);
}

namespace {

void Line(ostream& out, int depth, const string& text)
{
	for (int i = 0; i < depth; i++)
		out << "  ";
	out << text << "\n";
}

const char* BindingKeyword(EScopeBindingKind kind)
{
	switch (kind)
	{
	case SB_TYPE: return "type";
	case SB_TYPE_ALIAS: return "type-alias";
	case SB_ENUMERATOR: return "enumerator";
	case SB_FUNCTION: return "function";
	case SB_VARIABLE: return "variable";
	case SB_PARAMETER: return "parameter";
	case SB_NAMESPACE:
	case SB_NAMESPACE_ALIAS:
	case SB_CLASS_TEMPLATE:  // PA18: never printed by the PA11 dump
		break;
	}
	return 0;
}

string ScopeHeading(const Scope& scope)
{
	switch (scope.kind)
	{
	case SCOPE_NAMESPACE:
		if (!scope.parent)
			return "scope namespace <global>";
		return "scope namespace " +
			(scope.name.empty() ? string("<unnamed>") : scope.name);
	case SCOPE_TEMPLATE_PARAMS:
		return "scope template-parameters";
	case SCOPE_CLASS:
		return "scope class " + scope.name;
	case SCOPE_ENUM:
		return "scope enum " + scope.name;
	case SCOPE_FUNCTION:
		return "scope function " + scope.name;
	case SCOPE_BLOCK:
		break;
	}
	return "scope block";
}

void PrintScope(const Scope& scope, ostream& out, int depth)
{
	Line(out, depth, ScopeHeading(scope));
	for (size_t i = 0; i < scope.bindings.size(); i++)
	{
		const ScopeBinding& binding = scope.bindings[i];
		const char* keyword = BindingKeyword(binding.kind);
		if (!keyword)
			continue;  // namespaces print as child scopes, aliases never
		string line = string(keyword) + " " + binding.name + " " +
			DescribeType(binding.type);
		if (binding.kind == SB_ENUMERATOR)
			line += " " + RenderConstValue(binding.value);
		Line(out, depth + 1, line);
	}
	for (size_t i = 0; i < scope.children.size(); i++)
		PrintScope(*scope.children[i], out, depth + 1);
}

}  // namespace

void PrintTypesOutput(const Scope& global, ostream& out)
{
	Line(out, 0, "translation-unit");
	PrintScope(global, out, 1);
}
