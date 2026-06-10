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

// PA7/PA8 entity model: namespaces and the variables/functions/typedefs
// they declare. A SemaModel owns the Namespace tree of one translation
// unit; the Program (sema/program.h) owns every DeclaredEntity, shared
// across translation units when linked (3.5), as well as the other
// image objects. All other references are non-owning raw pointers, so
// bindings introduced by using-declarations and namespace aliases share
// the underlying objects (an entity prints only under the namespace
// that first declared it, and a redeclaration through any binding
// updates the one shared entity).

struct Namespace;

enum ESlotKind
{
	SLOT_VARIABLE,
	SLOT_FUNCTION,
	SLOT_TEMPORARY,  // lifetime-extended temporary (8.5.3)
	SLOT_STRING      // string-literal object (one per token)
};

enum EInitKind
{
	INIT_ZERO,    // zero-initialized (or never initialized)
	INIT_BYTES,   // constant object representation in init_bytes
	INIT_ADDRESS  // 64-bit image offset of init_target plus init_addend
};

// Anything that can be placed in the mock program image or targeted by
// a relocation: a variable or function entity, a temporary, or a
// string-literal object. Holds the typed initial-value facts the
// constant evaluator (5.19) and the image writer share.
struct ImageSlot
{
	ImageSlot()
		: kind(SLOT_VARIABLE), init_kind(INIT_ZERO), init_target(0),
		  init_addend(0), init_is_constant(false), is_constexpr(false),
		  defined_tu(-1), placed(false), offset(0)
	{}

	ESlotKind kind;
	TypePtr type;

	EInitKind init_kind;
	string init_bytes;             // INIT_BYTES, padded to the object size
	const ImageSlot* init_target;  // INIT_ADDRESS
	unsigned long long init_addend;
	// True when the recorded initializer was a constant expression, the
	// gate for lvalue-to-rvalue conversion in constant contexts (5.19).
	bool init_is_constant;
	bool is_constexpr;
	// Index of the translation unit whose definition recorded the value
	// (-1 if undefined); a constant read requires the initialization to
	// be visible, i.e. recorded by the reading translation unit.
	int defined_tu;

	bool placed;  // false: never defined, relocations resolve to 0
	unsigned long long offset;
};

enum ELinkage
{
	LNK_EXTERNAL,
	LNK_INTERNAL
};

// A namespace-scope variable or function (kind SLOT_VARIABLE or
// SLOT_FUNCTION). One object per linked entity program-wide.
struct DeclaredEntity : ImageSlot
{
	DeclaredEntity() : linkage(LNK_EXTERNAL), is_inline(false) {}

	string name;
	ELinkage linkage;
	bool is_inline;  // functions (7.1.2)
};

enum EBindingKind
{
	BK_VARIABLE,
	BK_FUNCTION,
	BK_NAMESPACE,  // member namespace or namespace alias: same object
	BK_TYPEDEF
};

// One member of a function overload set. `imported` marks entries a
// using-declaration brought in (7.3.3): they cannot be redeclared
// through this binding.
struct FunctionOverload
{
	FunctionOverload() : entity(0), imported(false) {}
	FunctionOverload(DeclaredEntity* entity, bool imported)
		: entity(entity), imported(imported) {}

	DeclaredEntity* entity;
	bool imported;
};

struct Binding
{
	Binding() : kind(BK_TYPEDEF), entity(0), target(0), imported(false) {}

	EBindingKind kind;
	DeclaredEntity* entity;             // BK_VARIABLE
	vector<FunctionOverload> overloads; // BK_FUNCTION
	Namespace* target;                  // BK_NAMESPACE
	TypePtr type;                       // BK_TYPEDEF
	// True when introduced by a using-declaration or namespace-alias-
	// definition rather than declared in the namespace itself: such a
	// name conflicts with a subsequent member declaration (7.3.3p8) and
	// is not extendable/reopenable (7.3.1, 7.3.1.2).
	bool imported;
};

struct Namespace
{
	Namespace() : is_inline(false), parent(0), unnamed_member(0) {}

	string name;  // empty for the global and unnamed namespaces
	bool is_inline;
	Namespace* parent;  // null for the global namespace

	// Every name declared in this namespace (members, aliases, names
	// introduced by using-declarations).
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

// The per-translation-unit namespace tree arena.
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

private:
	vector<unique_ptr<Namespace>> namespaces_;
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

// True when `outer` is `inner` or one of its ancestors (3.3.6: every
// namespace encloses itself).
bool NamespaceEncloses(const Namespace* outer, const Namespace* inner);

// The qualified path of `ns` from the global namespace ("A::B"; empty
// for the global namespace itself). Unnamed namespaces contribute an
// empty component, but entities under one have internal linkage and
// never use the path.
string NamespacePath(const Namespace* ns);

// True when `ns` or any of its ancestors is an unnamed namespace
// (3.5p4: members of such namespaces have internal linkage).
bool InsideUnnamedNamespace(const Namespace* ns);

// Writes the PA7 namespace description (recursively) to `out`.
void DescribeNamespace(ostream& out, const Namespace& ns);
