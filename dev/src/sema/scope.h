#pragma once

#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

using std::map;
using std::ostream;
using std::string;
using std::unique_ptr;
using std::vector;

#include "sema/type.h"

// PA11 scope model: the per-translation-unit scope tree with its
// declaration bindings, plus the named-type entity records the shared
// type nodes point at. One TypesModel owns everything; bindings and
// scopes reference each other through non-owning pointers, so names
// introduced by using-declarations and namespace aliases share the
// underlying scopes and types. The dump (PrintTypesOutput) renders a
// scope's bindings in first-binding order followed by its child scopes
// in creation order, which is exactly how the model stores them.

enum EScopeKind
{
	SCOPE_NAMESPACE,
	SCOPE_TEMPLATE_PARAMS,
	SCOPE_CLASS,
	SCOPE_ENUM,
	SCOPE_FUNCTION,
	SCOPE_BLOCK
};

enum EScopeBindingKind
{
	SB_TYPE,        // class or enumeration name
	SB_TYPE_ALIAS,  // typedef-name / alias-declaration / imported alias
	SB_ENUMERATOR,
	SB_FUNCTION,
	SB_VARIABLE,
	SB_PARAMETER,
	SB_NAMESPACE,       // member namespace (prints as a child scope)
	SB_NAMESPACE_ALIAS  // namespace-alias-definition (never printed)
};

// A typed value of the PA11 integral constant-expression subset. For
// signed types the 64 stored bits are the sign-extended value.
struct ConstValue
{
	ConstValue() : type(FT_INT), bits(0) {}
	ConstValue(EFundamentalType type_in, unsigned long long bits_in)
		: type(type_in), bits(bits_in) {}

	EFundamentalType type;
	unsigned long long bits;
};

struct Scope;
struct AstExpr;

// PA15 member access control (clause 11). Bindings outside class
// scopes stay MA_PUBLIC.
enum EMemberAccess
{
	MA_PUBLIC,
	MA_PROTECTED,
	MA_PRIVATE
};

struct ScopeBinding
{
	ScopeBinding() : kind(SB_VARIABLE), target(0), has_value(false),
	                 owner(0), home(0), c_linkage(false),
	                 access(MA_PUBLIC), is_mutable(false),
	                 is_thread_local(false) {}

	EScopeBindingKind kind;
	string name;
	TypePtr type;    // entity / alias target type (null for namespaces)
	Scope* target;   // SB_NAMESPACE / SB_NAMESPACE_ALIAS
	bool has_value;  // SB_ENUMERATOR and constant SB_VARIABLE
	ConstValue value;
	// The scope the binding was declared in (stamped by AddBinding; a
	// using-declaration import keeps the original owner). Powers the
	// PA12 canonical qualified names.
	const Scope* owner;
	// PA15: the scope the binding physically lives in (always stamped
	// by AddBinding); a using-declaration import is checked against the
	// importing class's access here.
	const Scope* home;
	// PA12 SB_FUNCTION overload set: the types declared for this name
	// beyond `type`, in declaration order (PA11 never populates it).
	vector<TypePtr> overloads;
	// PA12 anonymous-union members injected into a block scope (9.5p5):
	// the synthesized storage variable they live in.
	string anon_storage_name;
	TypePtr anon_storage_type;
	// PA14 SB_FUNCTION facts, indexed by overload position (entry 0 is
	// `type`, entry i+1 is `overloads[i]`): per-parameter default
	// argument expressions (null when absent; analyzed at call sites
	// while the AST is alive) and deleted-definition marking.
	vector<vector<const AstExpr*>> fn_defaults;
	vector<bool> fn_deleted;
	bool c_linkage;  // declared inside extern "C"
	// PA15 member facts. `access` is the declared access of a non-function
	// member (and of the first function overload); functions carry one
	// entry per overload position in the fn_* vectors below.
	EMemberAccess access;
	bool is_mutable;            // mutable non-static data member
	bool is_thread_local;       // declared thread_local
	vector<EMemberAccess> fn_access;
	vector<bool> fn_static;      // static member function
	vector<bool> fn_inline_def;  // defined in-class: weak, demand-emitted
	vector<bool> fn_adl_only;    // hidden friend: visible to ADL only
	vector<bool> fn_unwind_no;   // simple-noexcept declarator
};

struct Scope
{
	Scope() : kind(SCOPE_NAMESPACE), parent(0), unnamed_member(0),
	          class_base(0) {}

	EScopeKind kind;
	string name;  // empty for the global namespace, blocks, and
	              // template-parameter scopes
	Scope* parent;
	// PA15 single inheritance: the direct base class's member scope
	// (null otherwise); member lookup searches the base chain (10.2).
	Scope* class_base;

	vector<ScopeBinding> bindings;      // first-binding (print) order
	map<string, size_t> binding_index;  // name -> bindings position
	vector<Scope*> children;            // creation (print) order
	// Namespace scopes nominated by using-directives that lexically
	// appeared in this scope, including the implicit directives of
	// unnamed (7.3.1.1p1) and inline (7.3.1p9) member namespaces.
	vector<Scope*> using_directives;
	// 7.3.1.1p1: the unique unnamed member namespace, if any.
	Scope* unnamed_member;
};

// The per-translation-unit arena: scope tree, named-type entity
// records, and the entity -> member-scope association used by
// qualified lookup through class and scoped-enum names.
class TypesModel
{
public:
	TypesModel();

	Scope* global() const
	{
		return global_;
	}

	// Creates a scope and appends it to `parent`'s child (print) list.
	Scope* CreateScope(EScopeKind kind, const string& name, Scope* parent);

	// `scope` and `name` record the entity's structural identity
	// (declaring scope, bare declared name) alongside the display text.
	NamedTypeInfo* CreateNamedTypeInfo(const string& display,
	                                   const Scope* scope,
	                                   const string& name);

	// Every NamedTypeInfo is owned mutably by its model; Type nodes
	// carry a const view. The binder completes entities found through
	// types with this accessor.
	NamedTypeInfo* MutableInfo(const NamedTypeInfo* info);

	void SetMemberScope(const NamedTypeInfo* info, Scope* scope);
	Scope* MemberScope(const NamedTypeInfo* info) const;  // null if none
	// The entity a member scope belongs to (null for non-member scopes).
	const NamedTypeInfo* ScopeEntity(const Scope* scope) const;

private:
	vector<unique_ptr<Scope>> scopes_;
	vector<unique_ptr<NamedTypeInfo>> infos_;
	map<const NamedTypeInfo*, Scope*> member_scopes_;
	map<const Scope*, const NamedTypeInfo*> scope_entities_;
	Scope* global_;
};

// The binding of `name` declared in `scope` itself, or null.
ScopeBinding* FindOwnBinding(Scope& scope, const string& name);
const ScopeBinding* FindOwnBinding(const Scope& scope, const string& name);

// Appends a new binding; throws if the name is already bound in the
// scope (the callers merge redeclarations before adding).
ScopeBinding& AddBinding(Scope& scope, const ScopeBinding& binding);

// Appends a using-directive unless `nominated` is already nominated.
void AddUsingDirective(Scope& scope, Scope* nominated);

// The decimal rendering of a constant (signed types render the
// sign-extended value).
string RenderConstValue(const ConstValue& value);

// Writes the PA11 dump of one translation unit: the `translation-unit`
// line and the scope tree below it.
void PrintTypesOutput(const Scope& global, ostream& out);
