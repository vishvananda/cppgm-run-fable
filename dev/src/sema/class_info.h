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
	// PA21 9.5: an anonymous-union member row injected beside its
	// storage row (variant members are not default-initialized or
	// destroyed implicitly).
	bool from_union = false;
	// PA25 5.1.2: the closure field holding the captured `this`
	// pointer (stored as a pointer value, not a capture reference).
	bool captured_this = false;
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
	// Non-throwing facts: `unwind_no` merges the declared
	// specification with the derived body fact; `noexcept_decl` keeps
	// only the declared specification (the noexcept operator, 5.3.7).
	bool unwind_no;
	bool noexcept_decl = false;
	// The defining DK_SPECIAL_MEMBER_DEFINITION (mem-initializers and
	// body), or null for declaration-only / defaulted constructors.
	const AstDecl* definition;
	vector<const AstExpr*> defaults;  // per-parameter default arguments
	// 12.9 inheriting constructor: the base class whose constructor this
	// one forwards to (null for ordinary constructors).
	const NamedTypeInfo* inherited_base;

	// PA21: the constructor-template specialization this entry was
	// synthesized for (null for declared constructors); the lowering
	// mangles the C1/C2 object names with its template arguments.
	const struct FunctionSpecialization* tmpl_spec = 0;
	// The specialization's argument alias scope (the body and
	// mem-initializers bind under it).
	Scope* tmpl_param_scope = 0;

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
	ClassConversion()
		: is_explicit(false), access(MA_PUBLIC), decl(0), spec(0)
	{}

	string name;     // canonical member-scope binding name
	TypePtr result;  // conversion-type-id
	TypePtr type;    // declared function type (cv on the node)
	bool is_explicit;
	EMemberAccess access;
	// PA19: the declaring AST when defined in-class; the restricted
	// constant evaluator reads a single-return body from it.
	const struct AstDecl* decl;
	// PA22: the deduced conversion-template specialization behind a
	// synthesized entry (null for declared conversion functions); the
	// call site routes through its identity.
	const struct FunctionSpecialization* spec;
};

// PA17: one vtable slot (10.3). Slots inherit position from the base
// class; an override replaces the final overrider in place. A virtual
// destructor occupies two adjacent slots (complete, then deleting).
enum EVirtualSlotKind
{
	VS_METHOD,
	VS_DTOR_COMPLETE,
	VS_DTOR_DELETING
};

struct VirtualSlot
{
	VirtualSlot()
		: kind(VS_METHOD), owner(0), pure(false), is_final(false)
	{}

	EVirtualSlotKind kind;
	string name;          // member name ("~C" for destructor slots)
	// The final overrider's this-adjusted function type (the member
	// entry identity the lowering keys on). VS_METHOD only.
	TypePtr type;
	const Scope* owner;   // final overrider's class member scope
	bool pure;            // 10.4 pure-virtual declaration
	bool is_final;        // 10.3p4 method-level final
};

// PA26: one direct base-class subobject row - the base's class record,
// its byte offset inside the derived object, and the declared access.
// Bases lay out in declaration order; empty bases occupy no storage and
// sit at offset 0 (the empty-base optimization), every other base
// reserves its span before the first field.
struct ClassDirectBase
{
	ClassDirectBase() : cls(0), offset(0), access(MA_PUBLIC) {}

	const ClassInfo* cls;
	unsigned long long offset;
	EMemberAccess access;
};

struct ClassInfo
{
	ClassInfo()
		: entity(0), members(0), base(0), base_access(MA_PUBLIC),
		  is_union(false), is_empty(true), dsize(0), size(1), alignment(1),
		  is_aggregate(true), has_user_ctor(false), has_user_dtor(false),
		  dtor_deleted(false), dtor_access(MA_PUBLIC), dtor_definition(0),
		  dtor_unwind_no(false), bit_cursor(0),
		  implicit_ctor_built(false),
		  implicit_dtor_built(false), implicit_ctor_unwind_no(false),
		  implicit_dtor_unwind_no(false), dtor_user_declared(false),
		  has_user_copy_ctor(false), has_user_move_ctor(false),
		  has_user_copy_assign(false), has_user_move_assign(false),
		  specials_declared(false), copy_assign_index(-1),
		  move_assign_index(-1), copy_assign_deleted(false),
		  copy_assign_built(false), move_assign_built(false),
		  copy_assign_unwind_no(false), move_assign_unwind_no(false),
		  is_polymorphic(false), declares_virtual(false),
		  dtor_virtual(false), dtor_slot(-1), key_is_dtor(false),
		  key_defined_in_tu(false),
		  facts_version(0), facts_valid(0), facts_value(0)
	{}

	const NamedTypeInfo* entity;
	Scope* members;
	// The first direct base (the primary chain: vtable inheritance and
	// the polymorphic layer flow through it; it always sits at offset 0).
	const ClassInfo* base;
	EMemberAccess base_access;
	// PA26: every direct base in declaration order with its resolved
	// subobject offset (direct_bases[0].cls == base when a base exists).
	// Extra bases must stay non-polymorphic (polymorphic multiple
	// inheritance is PA27 territory).
	vector<ClassDirectBase> direct_bases;
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
	// PA21: declared constructor templates (deduced against argument
	// lists at construction sites; selected specializations synthesize
	// ClassCtor entries).
	vector<struct TemplateInfo*> ctor_templates;
	// PA22: declared conversion-function templates (deduced against
	// the required destination type at classification; selected
	// specializations synthesize ClassConversion entries).
	vector<struct TemplateInfo*> conversion_templates;
	// PA16 conversion functions in declaration order (12.3.2).
	vector<ClassConversion> conversions;

	// Layout cursor state used while the class is still open.
	unsigned long long bit_cursor;  // next free bit from the object start

	// Synthesized implicit special members (built on first demand).
	// PA24: the field-wise aggregate constructor synthesizes one
	// signature per distinct parameter cover (provided initializers
	// extended through the last non-scalar field).
	std::map<unsigned long long, bool> aggregate_ctor_covers;
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

	// --- PA17 polymorphic object-model facts (10.3) ---
	// A polymorphic class carries the single shared vpointer at offset
	// 0: inherited from a polymorphic direct base, or introduced by the
	// first textually `virtual` member (pre-scanned before layout so
	// fields declared ahead of the first virtual member still follow
	// the vpointer).
	bool is_polymorphic;
	bool declares_virtual;  // own members spell the `virtual` keyword
	// Vtable slot order: inherited base slots first (overriders replace
	// in place), then newly introduced slots in declaration order.
	vector<VirtualSlot> vslots;
	bool dtor_virtual;  // declared virtual or inherits a virtual dtor
	int dtor_slot;      // VS_DTOR_COMPLETE position (-1 when none)
	// Itanium-style key function: the first declared non-pure virtual
	// member (destructor included) without an in-class definition. The
	// vtable emits strong in the translation unit that defines it, weak
	// on demand when no key function exists.
	string key_name;    // empty when the class has no key function
	TypePtr key_type;   // declared (unadjusted) function type
	bool key_is_dtor;
	bool key_defined_in_tu;
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

	// Creation-order iteration (deterministic; the lowering walks it
	// for key-function vtable emission).
	const vector<const ClassInfo*>& All() const
	{
		return order_;
	}

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
	// The order-independent variant: instantiated user constructors
	// read their captured pattern bodies (whole-object elision).
	bool DefaultConstructionHasSyntacticEffects(const ClassInfo& info) const;

	// Whether default-initialization requires constructor code (a user
	// constructor, a default member initializer, or a base or member
	// that itself needs construction).
	bool NeedsConstruction(const ClassInfo& info) const;

	// The class record of a (possibly array-of-class) member type, or
	// null when the type holds no class subobject.
	const ClassInfo* MemberClass(const TypePtr& type) const;

private:
	bool ComputeDefaultConstructionEffects(const ClassInfo& info,
	                                       bool syntactic) const;

	map<const NamedTypeInfo*, unique_ptr<ClassInfo>> infos_;
	vector<const ClassInfo*> order_;  // creation order
};

// PA21 11.3p1: whether a friend-class grant covers `context` (a
// friend class template's anchor grants every specialization).
bool FriendClassMatches(const NamedTypeInfo* granted,
                        const NamedTypeInfo* context);

// PA21: whether `to` is `from` or one of its bases, including the
// extra direct bases beside the primary chain.
bool DerivedFromWithExtras(const ClassRegistry& classes,
                           const NamedTypeInfo* from,
                           const NamedTypeInfo* to);
// The link-based variant (class_record chain; no registry needed).
bool DerivedFromWithExtrasLinked(const NamedTypeInfo* from,
                                 const NamedTypeInfo* to);

// PA26: the derivation path from `from` down to its base `to` over the
// non-virtual base DAG (link-based through class_record/base_entity).
enum EBasePath
{
	BP_NONE,       // `to` is not `from` or a base of `from`
	BP_UNIQUE,     // exactly one subobject path
	BP_AMBIGUOUS   // `to` names more than one subobject (10.2)
};

// Resolves the path: `hops` receives the direct-base edge count and
// `offset` the summed byte offset of the unique path. An adjustment
// lowers as a single base-subobject projection with the total offset.
EBasePath BaseSubobjectPath(const NamedTypeInfo* from,
                            const NamedTypeInfo* to,
                            int& hops, unsigned long long& offset);

// PA26: `cls` and its transitive non-virtual bases, depth-first in
// declaration order, each class once. Conversion-function collection
// walks it (12.3.2 conversions are inherited from every base).
void CollectClassAndBases(const ClassInfo* cls,
                          vector<const ClassInfo*>& out);

// PA26: whether any transitive non-virtual base subobject of `cls`
// sits at a non-zero offset. A member-pointer value of such a class
// may carry a non-zero this-adjustment, so its call sites apply the
// dynamic adjustment; other classes keep the adjustment-free shape.
bool ClassHasDisplacedBase(const ClassInfo* cls);

// PA26 11.2: one derivation edge - the deriving class's record and the
// direct-base row index taken.
struct ClassBaseEdge
{
	const ClassInfo* derived;
	size_t base_index;
};

// The unique derivation path's edges from `from` down to its base
// `to`, outermost first, for base-specifier access checks. False when
// there is no unique edge path.
bool BaseAccessPath(const NamedTypeInfo* from, const NamedTypeInfo* to,
                    vector<ClassBaseEdge>& edges);

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

// PA17: the vtable slot of the virtual member function matching the
// declared (unadjusted) function signature - name, parameter list, and
// method cv-qualification (10.3p2) - or -1. Callers pass the static
// class of the object expression; inherited slots keep their base
// positions, so the index is dispatch-ready.
int FindVirtualSlotIndex(const ClassInfo& info, const string& name,
                         const TypePtr& declared);

// PA17 layout pre-scan: whether a class body textually declares a
// virtual member (drives vpointer reservation before fields lay out).
bool ClassBodyDeclaresVirtual(const AstDecl& decl);

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
