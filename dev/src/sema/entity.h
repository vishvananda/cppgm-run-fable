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

// PA7 entity model: namespaces and the variables/functions/typedefs
// they declare. SemaModel is the arena that owns every Namespace and
// DeclaredEntity for one translation unit; all other references are
// non-owning raw pointers, so bindings introduced by using-declarations
// and namespace aliases share the underlying objects (an entity prints
// only under the namespace that first declared it, and a redeclaration
// through any binding updates the one shared entity).

struct Namespace;

// A namespace-scope variable or function. Whether it is a variable or
// a function is the kind of the binding that names it (equivalently:
// whether the declared type is a function type).
struct DeclaredEntity
{
	string name;
	TypePtr type;
};

enum EBindingKind
{
	BK_VARIABLE,
	BK_FUNCTION,
	BK_NAMESPACE,  // member namespace or namespace alias: same object
	BK_TYPEDEF
};

struct Binding
{
	Binding() : kind(BK_TYPEDEF), entity(0), target(0) {}

	EBindingKind kind;
	DeclaredEntity* entity;  // BK_VARIABLE / BK_FUNCTION
	Namespace* target;       // BK_NAMESPACE
	TypePtr type;            // BK_TYPEDEF
};

struct Namespace
{
	Namespace() : is_inline(false), parent(0), unnamed_member(0) {}

	string name;  // empty for the global and unnamed namespaces
	bool is_inline;
	Namespace* parent;  // null for the global namespace

	// Every name declared in this namespace (members, aliases, names
	// introduced by using-declarations). One binding per name: PA7
	// inputs are well-formed and overload sets have one entry.
	map<string, Binding> bindings;

	// Print lists in order of first declaration; only entities and
	// member namespaces first declared here (never aliases or
	// using-declaration bindings).
	vector<const DeclaredEntity*> variables;
	vector<const DeclaredEntity*> functions;
	vector<Namespace*> members;

	// 7.3.1.1p1: every unnamed-namespace-definition in this namespace
	// extends the one unique unnamed member.
	Namespace* unnamed_member;

	// Namespaces nominated by using-directives that lexically appeared
	// in this namespace, in order, including the implicit directives of
	// unnamed (7.3.1.1p1) and inline (7.3.1p9) member namespaces.
	vector<Namespace*> using_directives;
};

class SemaModel
{
public:
	SemaModel();

	Namespace* global() const
	{
		return global_;
	}

	Namespace* CreateNamespace(const string& name, bool is_inline,
	                           Namespace* parent);
	DeclaredEntity* CreateEntity(const string& name, const TypePtr& type);

private:
	vector<unique_ptr<Namespace>> namespaces_;
	vector<unique_ptr<DeclaredEntity>> entities_;
	Namespace* global_;
};

// Creates a new member namespace of `parent` (empty `name` means the
// unnamed member) and wires the semantic facts of 7.3.1: print-list
// position, the name binding (named case), the unique-unnamed-member
// slot, and the implicit using-directive for unnamed and inline
// members.
Namespace* AddMemberNamespace(SemaModel& model, Namespace& parent,
                              const string& name, bool is_inline);

// Appends a using-directive if `nominated` is not already nominated in
// `ns` (a repeated directive adds nothing to any lookup).
void AddUsingDirective(Namespace& ns, Namespace* nominated);

// Writes the PA7 namespace description (recursively) to `out`.
void DescribeNamespace(ostream& out, const Namespace& ns);
