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

// PA16: the special-member role of a constructor (12.8p2/p3).
enum ECtorKind
{
	CK_ORDINARY,
	CK_COPY,
	CK_MOVE
};

// One declared constructor (12.1). The implicit default constructor is
// synthesized on first use and does not appear here; implicitly
// declared copy/move constructors append at class completion with
// `implicit` set.
struct ClassCtor
{
	ClassCtor()
		: inherited_built(false), access(MA_PUBLIC), is_explicit(false),
		  deleted(false), defaulted(false), unwind_no(false), definition(0),
		  inherited_base(0), kind(CK_ORDINARY), implicit(false),
		  ignore_in_overload(false), built(false), built_unwind_no(false)
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

	// --- PA16 special-member facts ---
	ECtorKind kind;   // classified by parameter shape (12.8p2/p3)
	bool implicit;    // implicitly declared at class completion
	// A defaulted move constructor defined as deleted is ignored by
	// overload resolution (12.8p9).
	bool ignore_in_overload;
	// 8.4.2p4: defaulted after its first declaration is user-provided
	// (non-trivial), though it still synthesizes the implicit body.
	bool defaulted_outside;
	bool built;            // synthesized definition pushed (implicit/defaulted)
	bool built_unwind_no;  // synthesized body cannot throw
};

// PA16: one declared conversion function (12.3.2), bound in the class
// member scope under its canonical "operator <type>" name.
struct ClassConversion
{
	ClassConversion() : is_explicit(false), access(MA_PUBLIC) {}

	string name;     // canonical member-scope binding name
	TypePtr result;  // conversion-type-id
	TypePtr type;    // declared function type (cv on the node)
	bool is_explicit;
	EMemberAccess access;
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
		  implicit_dtor_unwind_no(false), dtor_user_declared(false),
		  has_user_copy_ctor(false), has_user_move_ctor(false),
		  has_user_copy_assign(false), has_user_move_assign(false),
		  specials_declared(false), copy_assign_index(-1),
		  move_assign_index(-1), copy_assign_deleted(false),
		  copy_assign_built(false), move_assign_built(false),
		  copy_assign_unwind_no(false), move_assign_unwind_no(false),
		  facts_version(0), facts_valid(0), facts_value(0)
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
	// PA16 conversion functions in declaration order (12.3.2).
	vector<ClassConversion> conversions;

	// Layout cursor state used while the class is still open.
	unsigned long long bit_cursor;  // next free bit from the object start

	// Synthesized implicit special members (built on first demand).
	bool aggregate_ctor_built;
	bool implicit_ctor_built;
	bool implicit_dtor_built;
	bool implicit_ctor_unwind_no;
	bool implicit_dtor_unwind_no;

	// --- PA16 copy/move special-member state ---
	bool dtor_user_declared;  // any destructor declaration (incl. = default)
	bool has_user_copy_ctor;
	bool has_user_move_ctor;
	bool has_user_copy_assign;
	bool has_user_move_assign;
	bool specials_declared;   // implicit copy/move members appended
	// Implicitly declared assignment operators: their overload position
	// in the class's "operator =" member binding (-1 when absent).
	int copy_assign_index;
	int move_assign_index;
	bool copy_assign_deleted;
	bool copy_assign_built;
	bool move_assign_built;
	bool copy_assign_unwind_no;
	bool move_assign_unwind_no;
	// Lazily memoized recursive class facts (triviality and
	// construction/destruction queries). Out-of-class special-member
	// definitions can change the underlying facts after class
	// completion; each such mutation bumps the global facts version
	// (InvalidateClassFacts) and stale memos recompute on next query.
	mutable unsigned long long facts_version;
	mutable unsigned facts_valid;
	mutable unsigned facts_value;
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
	bool ComputeDefaultConstructionEffects(const ClassInfo& info) const;

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

// Finishes the layout: applies any class-head alignas request,
// rounds the size, and stamps the entity's size/alignment facts.
void FinishClassLayout(ClassInfo& info, NamedTypeInfo& entity,
                       unsigned long long min_alignment);

// --- queries ----------------------------------------------------------

// The field row of `name` declared in this class (no base search).
const ClassField* FindClassField(const ClassInfo& info, const string& name);

// The constructor overload position whose parameter list matches
// `type`, or -1 (used for deterministic low-name overload suffixes).
int ClassCtorIndex(const ClassInfo& info, const TypePtr& type);

// 12.8p2/p3: the special-member role of a declared constructor of the
// class entity (first parameter a reference to the class, every later
// parameter defaulted).
ECtorKind ClassifyCtorKind(const NamedTypeInfo* entity,
                           const ClassCtor& ctor);

// --- PA16 triviality facts (9p6, 12.8) --------------------------------
// All traverse the base/member subobject tree through the entities'
// class_record links, so they need completed classes only. Results are
// memoized per class (ClassInfo::facts_*); a post-completion mutation
// of the underlying facts must call InvalidateClassFacts.

// Invalidates every memoized class fact. Called when an out-of-class
// special-member definition changes a completed class's facts
// (BindQualifiedSpecialMember).
void InvalidateClassFacts();

// The class record of a (possibly array-of-class) subobject type, or
// null (link-based; no registry needed).
const ClassInfo* SubobjectClass(const TypePtr& type);

bool ClassHasTrivialDtor(const ClassInfo& info);
bool ClassHasTrivialCopyCtor(const ClassInfo& info);
bool ClassHasTrivialMoveCtor(const ClassInfo& info);
bool ClassHasTrivialCopyAssign(const ClassInfo& info);
bool ClassHasTrivialMoveAssign(const ClassInfo& info);
bool ClassTriviallyCopyable(const ClassInfo& info);

// The PA16 LowIR boundary classification. Parameters pass as direct
// `obj<SxA>` payloads when destruction and moves are trivial (the
// caller constructs the argument object; the callee copies the
// payload); returns additionally require a trivial copy constructor
// (a direct return byte-copies the result). Everything else passes
// by_address / returns through indirect_result.
bool ClassParamDirect(const ClassInfo& info);
bool ClassReturnDirect(const ClassInfo& info);
