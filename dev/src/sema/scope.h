#pragma once

#include <deque>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

using std::deque;
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
	SB_NAMESPACE_ALIAS,  // namespace-alias-definition (never printed)
	// PA18: a class-template name (the template itself, not a type);
	// `templ` holds the template record. Function templates ride on
	// SB_FUNCTION bindings through `fn_templates` instead so they
	// overload with ordinary functions.
	SB_CLASS_TEMPLATE,
	// PA19: a variable-template name; `templ` holds the record and
	// uses resolve through the template-id seam to per-key constant
	// bindings.
	SB_VARIABLE_TEMPLATE
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

// PA15 member access control (clause 11). Bindings outside class
// scopes stay MA_PUBLIC.
enum EMemberAccess
{
	MA_PUBLIC,
	MA_PROTECTED,
	MA_PRIVATE
};

struct TemplateInfo;
struct FunctionSpecialization;

struct ScopeBinding
{
	ScopeBinding() : kind(SB_VARIABLE), target(0), has_value(false),
	                 no_object(false), param_index(-1), owner(0),
	                 home(0), c_linkage(false), access(MA_PUBLIC),
	                 is_mutable(false), is_thread_local(false),
	                 templ(0), fn_self_spec(0) {}

	EScopeBindingKind kind;
	string name;
	TypePtr type;    // entity / alias target type (null for namespaces)
	Scope* target;   // SB_NAMESPACE / SB_NAMESPACE_ALIAS
	bool has_value;  // SB_ENUMERATOR and constant SB_VARIABLE
	// PA23 14.6.4.1: the constant arrived with an instantiated
	// out-of-class member definition, which sits after every
	// parse-scope use in instantiation order. Only instantiated
	// bodies fold it; parse-scope reads keep loading the storage.
	bool value_from_def = false;
	// PA26: an instantiated const function-pointer member definition -
	// reads fold to the address of a symbolic (declare-only) function
	// named by the member, the reference presentation; the storage
	// keeps its dynamic initialization.
	bool fn_pointer_fold = false;
	// PA19: a constant binding with no object behind it (a non-type
	// template parameter): every use folds to `value`; odr-use
	// (address, reference binding) is ill-formed. In an abstract
	// pattern scope the binding has no value yet and `param_index`
	// names its template-parameter slot instead (-1 otherwise).
	bool no_object;
	int param_index;
	ConstValue value;
	// PA23: a variable-template specialization binding carries its
	// typed template identity (the template and resolved arguments)
	// so the lowering mangles the template-id form.
	const TemplateInfo* var_spec_template = 0;
	vector<TemplateArg> var_spec_args;
	// PA19 packs. A template parameter pack's alias binding carries
	// its elements (types or values); a function parameter pack's
	// binding additionally carries the expanded parameter slot names
	// (aligned with pack_args). Only `...` expansions and sizeof...
	// may use a pack binding by name.
	bool is_pack = false;
	vector<TemplateArg> pack_args;
	vector<string> pack_param_names;
	// A pack-element binding synthesized during one expansion step:
	// the lookup name is the pack's, but the entity behind it is the
	// expanded slot named here.
	string pack_element_name;
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
	// 7.5p6: which overload slots were declared with C language
	// linkage (at most one function per name may be).
	vector<bool> fn_c_linkage;
	// PA15 member facts. `access` is the declared access of a non-function
	// member (and of the first function overload); functions carry one
	// entry per overload position in the fn_* vectors below.
	EMemberAccess access;
	bool is_mutable;            // mutable non-static data member
	bool is_thread_local;       // declared thread_local
	vector<EMemberAccess> fn_access;
	vector<bool> fn_static;      // static member function
	vector<bool> fn_inline_def;  // defined in-class: weak, demand-emitted
	// 7.1.2p2: a function is inline if any declaration spells
	// `inline`, so a later out-of-class definition emits weak even
	// without respelling it.
	vector<bool> fn_declared_inline;
	vector<bool> fn_adl_only;    // hidden friend: visible to ADL only
	// Non-throwing facts per overload. `fn_unwind_no` merges the
	// declared specification with the derived body fact (unwind-region
	// elision); `fn_noexcept_decl` keeps only the declared
	// specification, which the noexcept operator reads (5.3.7).
	vector<bool> fn_unwind_no;
	vector<bool> fn_noexcept_decl;
	// PA16: explicitly-defaulted special member functions (8.4.2); the
	// binder synthesizes their definitions like the implicit ones.
	vector<bool> fn_defaulted;
	// PA16: per-overload declaring scope. A using-declaration merges
	// imported overloads into the target's set; each entry keeps the
	// scope that really declared it (null falls back to `owner`).
	vector<const Scope*> fn_owner;
	// PA18: per-overload declared parameter names (first declaration
	// wins per slot); an out-of-class definition with unnamed
	// parameters falls back to them for its lowered slot names.
	vector<vector<string>> fn_param_names;
	// PA18 templates. SB_CLASS_TEMPLATE: the class template this name
	// declares. SB_FUNCTION: the function templates declared under
	// this name beside the ordinary overloads (using-declaration
	// imports copy the binding and carry them along).
	TemplateInfo* templ;
	vector<TemplateInfo*> fn_templates;
	// PA18: set on a FunctionSpecialization's own `self` binding so an
	// explicit template-id id-expression carries its identity.
	const FunctionSpecialization* fn_self_spec;
	// PA22: monotonic declaration order (stamped by AddBinding).
	// Dependent-name lookup during instantiation compares it against
	// the owning template's capture point so a later ordinary
	// declaration cannot shadow ADL (14.6.4).
	size_t seq = 0;
};

// The declaration-order stamp the next AddBinding will use; a template
// capture records it as its definition point.
size_t CurrentBindingSeq();

struct Scope
{
	Scope() : kind(SCOPE_NAMESPACE), parent(0), class_base(0),
	          base_dependent(false), entity(0), unnamed_member(0) {}

	EScopeKind kind;
	string name;  // empty for the global namespace, blocks, and
	              // template-parameter scopes
	Scope* parent;
	// PA25: the function-template specialization a SCOPE_FUNCTION
	// body belongs to (local-entity manglings spell it).
	const struct FunctionSpecialization* fn_spec = 0;
	// PA15: the first direct base class's member scope (null
	// otherwise); member lookup searches the base DAG (10.2).
	Scope* class_base;
	// PA26: the member scopes of the extra direct bases, in declaration
	// order. Lookup searches them beside `class_base`; a name found in
	// more than one sibling subobject is ambiguous.
	vector<Scope*> class_extra_bases;
	// PA18 14.6.2p3: the base was dependent in the template pattern;
	// unqualified lookup does not search it (qualified and member
	// -access lookup still do).
	bool base_dependent;
	// PA18: the named-type entity this member scope belongs to (set by
	// SetMemberScope; null for non-member scopes). The lowering's
	// symbol mangling reads the entity's template identity from here.
	const NamedTypeInfo* entity;
	// The composed type of a SCOPE_FUNCTION body scope (null on
	// synthesized helper scopes). Local-entity mangling (5.1.7) needs
	// the owning overload's identity, which a name lookup cannot give.
	TypePtr fn_type;
	// PA22 14.6.4 subset: set on instantiation argument-alias scopes to
	// the owning template's capture stamp. Unqualified lookup under
	// such a scope ignores namespace-scope objects declared after it,
	// so a later ordinary value cannot shadow ADL in a re-bound
	// pattern.
	size_t dependent_seq_limit = 0;

	// A deque so AddBinding never moves existing entries: lookup
	// results are held as ScopeBinding* across analysis that can
	// declare new entities in the same scope (e.g. a closure type
	// bound while the enclosing call's candidates are in hand).
	deque<ScopeBinding> bindings;       // first-binding (print) order
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
