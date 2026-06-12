#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::unique_ptr;
using std::vector;

#include "sema/scope.h"
#include "sema/type.h"

// PA15 class metadata: one ClassInfo per completed class entity holding
// the object-model facts the PA14 dump never needed - field offsets,
// bit-field packing, the single direct base, constructors/destructor,
// friends, and aggregate-ness. The semantic binder owns the records
// (through the per-unit ClassRegistry); the lowering only reads the
// resolved facts copied onto SemNodes plus the layout queries here.

struct AstDecl;
struct AstExpr;
struct AstInitializer;

// One non-static data member with its resolved layout. Anonymous
// struct/union members inject their fields into the enclosing class
// record with adjusted offsets, so every field row is addressable.
struct ClassField
{
	ClassField()
		: offset(0), is_bit_field(false), bit_offset(0), bit_width(0),
		  is_mutable(false), access(MA_PUBLIC), default_init(0)
	{}

	string name;  // empty for unnamed bit-field padding rows
	TypePtr type;
	unsigned long long offset;  // bytes from the complete object start
	bool is_bit_field;
	unsigned long long bit_offset;  // within the unit at `offset`
	unsigned long long bit_width;
	bool is_mutable;
	EMemberAccess access;
	// 12.6.2: brace-or-equal-initializer (owned by the AST).
	const AstInitializer* default_init;
};

// One declared constructor (12.1). The implicit default constructor is
// synthesized on first use and does not appear here.
struct ClassCtor
{
	ClassCtor()
		: access(MA_PUBLIC), is_explicit(false), deleted(false),
		  defaulted(false), unwind_no(false), definition(0),
		  inherited_base(0), inherited_built(false)
	{}

	vector<string> param_names;  // declared parameter names
	bool inherited_built;        // forwarding definition synthesized

	TypePtr type;  // function type over the declared parameters
	EMemberAccess access;
	bool is_explicit;
	bool deleted;
	bool defaulted;
	bool unwind_no;  // simple-noexcept declarator
	// The defining DK_SPECIAL_MEMBER_DEFINITION (mem-initializers and
	// body), or null for declaration-only / defaulted constructors.
	const AstDecl* definition;
	vector<const AstExpr*> defaults;  // per-parameter default arguments
	// 12.9 inheriting constructor: the base class whose constructor this
	// one forwards to (null for ordinary constructors).
	const NamedTypeInfo* inherited_base;
};

struct ClassInfo
{
	ClassInfo()
		: entity(0), members(0), base(0), base_access(MA_PUBLIC),
		  is_union(false), is_empty(true), dsize(0), size(1), alignment(1),
		  is_aggregate(true), has_user_ctor(false), has_user_dtor(false),
		  dtor_deleted(false), dtor_access(MA_PUBLIC), dtor_definition(0),
		  dtor_unwind_no(false), bit_cursor(0), aggregate_ctor_built(false),
		  implicit_ctor_built(false),
		  implicit_dtor_built(false), implicit_ctor_unwind_no(false),
		  implicit_dtor_unwind_no(false)
	{}

	const NamedTypeInfo* entity;
	Scope* members;
	const ClassInfo* base;  // single direct base subobject (offset 0)
	EMemberAccess base_access;
	vector<ClassField> fields;  // declaration order
	bool is_union;
	bool is_empty;  // no fields and no non-empty base
	// Occupied bytes (0 for an empty class); sizeof rounds dsize up to
	// the alignment, with a 1-byte minimum (9p4).
	unsigned long long dsize;
	unsigned long long size;
	unsigned long long alignment;
	bool is_aggregate;  // 8.5.1p1 (C++11: default member inits disqualify)
	bool has_user_ctor;
	bool has_user_dtor;
	vector<ClassCtor> ctors;
	bool dtor_deleted;
	EMemberAccess dtor_access;
	const AstDecl* dtor_definition;  // user destructor body (null: implicit)
	bool dtor_unwind_no;
	// 11.3 friends: entities of friend classes (forward declarations
	// included) and the declared (scope, name) of friend functions.
	vector<const NamedTypeInfo*> friend_classes;
	vector<std::pair<const Scope*, string>> friend_functions;

	// Layout cursor state used while the class is still open.
	unsigned long long bit_cursor;  // next free bit from the object start

	// Synthesized implicit special members (built on first demand).
	bool aggregate_ctor_built;
	bool implicit_ctor_built;
	bool implicit_dtor_built;
	bool implicit_ctor_unwind_no;
	bool implicit_dtor_unwind_no;
};

// The per-translation-unit class record arena, owned by the SemUnit.
class ClassRegistry
{
public:
	ClassInfo& Create(const NamedTypeInfo* entity);
	ClassInfo* Find(const NamedTypeInfo* entity);
	const ClassInfo* Find(const NamedTypeInfo* entity) const;

	// Whether destruction of an object of this class requires code (a
	// user destructor anywhere in the base/member subobject tree).
	bool NeedsDestruction(const ClassInfo& info) const;
	// Whether destroying / default-constructing an object performs any
	// observable work: a user-provided body with statements, a member
	// initializer, or a subobject chain that does. The reference
	// emission elides subobject constructor/destructor calls inside
	// synthesized lifetime code when the whole chain is empty.
	bool DestructionHasEffects(const ClassInfo& info) const;
	bool DefaultConstructionHasEffects(const ClassInfo& info) const;

	// Whether default-initialization requires constructor code (a user
	// constructor, a default member initializer, or a base or member
	// that itself needs construction).
	bool NeedsConstruction(const ClassInfo& info) const;

	// The class record of a (possibly array-of-class) member type, or
	// null when the type holds no class subobject.
	const ClassInfo* MemberClass(const TypePtr& type) const;

private:
	map<const NamedTypeInfo*, unique_ptr<ClassInfo>> infos_;
};

// --- layout -----------------------------------------------------------

// Starts the layout of a freshly opened class: places the direct base
// subobject at offset 0 and seeds the cursor and alignment.
void BeginClassLayout(ClassInfo& info);

// Appends one ordinary field at the next correctly aligned offset
// (9.2p13 declaration order; unions overlay at 0). Returns the row.
ClassField& LayoutField(ClassInfo& info, const ClassField& field);

// Appends one bit-field (9.6, Itanium allocation: the field packs into
// the next aligned unit of its declared type that can hold it). A
// zero-width unnamed field only realigns the cursor.
ClassField& LayoutBitField(ClassInfo& info, const ClassField& field);

// Finishes the layout: rounds the size and stamps the entity's
// size/alignment facts.
void FinishClassLayout(ClassInfo& info, NamedTypeInfo& entity);

// --- queries ----------------------------------------------------------

// The field row of `name` declared in this class (no base search).
const ClassField* FindClassField(const ClassInfo& info, const string& name);

// The constructor overload position whose parameter list matches
// `type`, or -1 (used for deterministic low-name overload suffixes).
int ClassCtorIndex(const ClassInfo& info, const TypePtr& type);
