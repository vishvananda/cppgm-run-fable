#pragma once

#include <deque>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

using std::deque;
using std::map;
using std::ostream;
using std::set;
using std::string;
using std::vector;

#include "sema/scope.h"
#include "sema/sem_node.h"
#include "sema/type.h"

// PA14 program assembly: the cross-translation-unit entity registry
// (globals, functions, string-literal objects) and the canonical
// top-level LowIR emission. Function bodies are lowered by
// FunctionLowerer (lower_function.h) against this registry.

// PA27: one hidden trailing pointer of a lowered signature (the
// virtual-base ABI; computed in lower_vbase.cpp - see pa27/plan.md).
enum EHiddenParamKind
{
	HP_VTT,    // construction-vtable table pointer (polymorphic C2/D2)
	HP_VBPTR,  // own vbase-table entry address (base entries, methods)
	HP_PVBPTR  // a declared parameter's carried vbase-table entry
};

struct HiddenParam
{
	HiddenParam() : kind(HP_VTT), param_index(0), vbase_index(0), cls(0) {}

	EHiddenParamKind kind;
	size_t param_index;  // HP_PVBPTR: the declared parameter it serves
	size_t vbase_index;  // table index inside `cls`
	const struct ClassInfo* cls;  // the class whose table entry rides
	string low_name;     // "__vtt" / "__vbptrN" / "__pvbptrK"
};

struct LowFunctionInfo
{
	LowFunctionInfo()
		: scope(0), is_main(false), defined(false), used(false),
		  deleted(false), c_linkage(false), unwind_no(false),
		  internal(false), weak(false), is_method(false), definition(0),
		  index(0)
	{}

	const Scope* scope;  // declaring namespace or class scope
	string name;         // declared name
	TypePtr type;
	string low_name;     // LowIR symbol (no sigil)
	string object_name;  // Itanium spelling ("" when omitted)
	bool is_main;
	bool defined;
	bool used;
	bool deleted;
	bool c_linkage;
	bool unwind_no;
	bool internal;
	// PA15: in-class definitions emit weak and only on demand; methods
	// carry the hidden `this` first parameter. `demand_strong` keeps
	// the demand gating but prints a strong binding (an explicit
	// -specialization definition).
	bool weak;
	bool demand_strong = false;
	bool is_method;
	// PA34 hosted collapse: some registered definition of this entry
	// spelled its signature through a collapsed _FloatN alias; scopes
	// the duplicate-inline-definition tolerance.
	bool alias_collapsed = false;
	// Constructor/destructor ABI entry: "C1"/"C2"/"D1"/"D2" ("" for
	// ordinary functions); C1/D1 definitions emit an alias for the
	// identical base entry.
	string special_code;
	string alias_object;
	string role;  // role=init / role=fini helpers
	// PA18: the function-template specialization this entry emits
	// (null for ordinary functions); the object name mangles from it.
	const FunctionSpecialization* fn_spec = 0;
	// PA18 14.7.2p8: emitted because an explicit instantiation
	// definition names its class (`object_root=yes`).
	bool object_root = false;
	// PA21 14.7.2p10: named by an `extern template` declaration - the
	// strong definition lives in another translation unit, so local
	// uses reference an external declare even though a weak body was
	// instantiated.
	bool extern_suppressed = false;
	// PA20: a hidden-friend definition (deferred, namespace scope):
	// its references count as semantic demand, so its body lowers even
	// unused. Weak constexpr/inline namespace functions do not.
	bool friend_def = false;
	const SemNode* definition;
	size_t index;        // position in functions_ (demand rescan key)
	string body_text;    // lowered definition text
	// PA27: the hidden trailing pointers of the lowered signature
	// (computed once on first use by HiddenSignatureParams).
	vector<HiddenParam> hidden;
	bool hidden_ready = false;
	// PA27: the lowered body carries a null-guarded displaced-base
	// adjustment; callers under cleanups guard the call lazily (the
	// eager segment scan keeps the sema unwind fact).
	bool guarded_body = false;
};

// PA27 hidden vbase-pointer ABI (lower_vbase.cpp). The class whose
// vbase table a parameter type carries - reference-to-class carries
// the full table, pointer-to-class (also behind a reference) the
// collapsed table (entries inside a direct virtual base collapse into
// it). Null when the type carries none.
const ClassInfo* ParamVBaseClass(const TypePtr& param, bool& collapsed);

// The carried table indices of one such parameter class.
void ParamCarriedEntries(const ClassInfo& cls, bool collapsed,
                         vector<size_t>& out);

// The hidden trailing pointers of a function's signature (cached).
const vector<HiddenParam>& HiddenSignatureParams(LowFunctionInfo& info);

// The type-only variant for indirect call signatures (function
// pointers, member pointers, vtable dispatch).
void HiddenParamsForType(const TypePtr& fn_type, bool is_method,
                         vector<HiddenParam>& out);

struct LowGlobalInfo
{
	LowGlobalInfo()
		: dynamic_init(false), type(), defined(false), used(false),
		  internal(false), is_thread_local(false), c_linkage(false),
		  node(0)
	{}

	// PA15: the initializer is not a constant expression; the object
	// zero-fills statically and @__cppgm_init stores the value.
	bool dynamic_init;

	TypePtr type;
	string low_name;
	string object_name;
	bool defined;
	bool used;
	bool internal;
	// PA36 3.5p3: some declaration spelled `extern`, so a later const
	// definition keeps external linkage.
	bool extern_declared = false;
	// PA18: an instantiated static-data-member definition (weak,
	// external).
	bool weak = false;
	// PA19: the binding carries a recorded constant value, so every
	// read folds and the stored image is unobservable.
	bool folded_const = false;
	// PA20: the evaluated constant image of an object-valued constexpr
	// definition; weak (static-member) definitions and initializer-less
	// storage definitions emit their flattened items from it.
	shared_ptr<const struct ConstObject> image;
	// Image render attempt, made before the init actions are dropped
	// (0 untried / 1 rendered into image_text / 2 unrenderable, the
	// dynamic-init paths take over).
	int image_state = 0;
	string image_text;
	bool is_thread_local;
	bool c_linkage;
	const SemNode* node;  // defining SN_VARIABLE (init children)
	string init_text;     // rendered definition text
};

struct LowStringLiteral
{
	string low_name;
	TypePtr element;
	string bytes;  // object representation including the terminator
};

// PA17: one polymorphic class's vtable global. Emitted strong when the
// class's key function is defined in this program, weak on demand when
// no key function exists, and as an external declare when the key
// function lives in another translation unit.
struct LowVTableInfo
{
	LowVTableInfo() : cls(0), used(false), strong(false), rendered(false)
	{}

	const ClassInfo* cls;
	string low_name;     // <Class>__vtable
	string object_name;  // _ZTV<class encoding>
	bool used;
	bool strong;    // key function defined in this program
	bool rendered;
	string text;    // rendered definition ("" for declare-only)
	// PA27: the class's full vtable group rides the primary entry -
	// secondary views, and (for polymorphic virtual-base classes) the
	// construction vtables and the VTT. `group_used` re-renders the
	// entry with the group when the demand arrives late.
	bool group_used = false;
	bool group_rendered = false;
	string vtt_low_name;  // "" until named
};

class LowerProgram
{
public:
	LowerProgram();

	// PA29: this program is one separately-compiled translation unit;
	// whole-program-only folds must stay off because sibling units are
	// not visible at compile time.
	void SetSeparateCompilation() { separate_compilation_ = true; }
	bool SeparateCompilation() const { return separate_compilation_; }
	void SetOptimizeLevel(int level) { optimize_level_ = level; }
	int OptimizeLevel() const { return optimize_level_; }

	// Registers one bound translation unit's namespace-scope items.
	void AddUnit(const SemUnit& unit);
	// PA32: demand-marks destructors whose effect-free invocations
	// emitted no cleanup code (separate compilation only).
	void DemandElidedDtorUses(const SemUnit& unit);

	// Renders globals, lowers function bodies, and writes the
	// canonical LowIR program.
	void Write(ostream& out);

	// --- references from lowered code (mark the entity used) ---
	// The "@name" spelling of a namespace-scope object.
	string GlobalRef(const Scope* scope, const string& name);
	// The "@name" spelling of the (scope, name, type) overload.
	string FunctionRef(const Scope* scope, const string& name,
	                   const TypePtr& type,
	                   const FunctionSpecialization* spec = 0);
	// The "@name" spelling of a method / constructor / destructor
	// callee node (PA15: demand-marks the weak definition).
	string MemberFunctionRef(const SemNode& callee);
	// PA27: the registry entry a direct call resolves to (hidden
	// vbase-pointer signatures), without demanding it.
	LowFunctionInfo& CalleeEntryInfo(const SemNode& callee);
	// PA27: the lowered callee body carries a null-guarded
	// displaced-base adjustment (lazy dispatch-region wrapping).
	bool CalleeGuardedBody(const SemNode& callee);
	// PA27 comdat pairing: whether the lowering context (a weak
	// constructor/destructor) pairs the demanded subobject entry's
	// complete twin (lower_vbase.cpp).
	bool ContextPairsSubobjectEntry(const ClassInfo* cls);
	// The alias lines of the emitted definitions (lower_vbase.cpp owns
	// the PA27 base-entry suppression).
	void AppendAliasSection(vector<string>& section);
	// PA29 5.5: the "@name" of the internal forwarding thunk a pointer
	// to a virtual member function carries (dispatches through the
	// object's vpointer); "" for non-virtual members.
	string MemberPointerThunkRef(const SemNode& member);
	// The registered definition behind a method callee (null when
	// none): the conversion-elision check reads its body shape.
	const SemNode* MemberDefinitionFor(const SemNode& callee);
	// The "@__strlit__N" object of a string-literal node.
	string StringLiteralRef(const SemNode& node);
	// Registers a namespace-scope object declaration (also used for
	// block-scope extern declarations naming the global entity).
	void RegisterGlobal(const SemNode& item);
	// --- PA20 function-local statics ---
	// Whether (scope, name) already resolves to a registered global.
	bool HasGlobal(const Scope* scope, const string& name) const;
	// Hoists a local static to a global under its block-scope entity
	// key (internal, or weak inside a vague-linkage definition).
	LowGlobalInfo& RegisterLocalStatic(const SemNode& item,
	                                   const string& base_name,
	                                   bool weak);
	// The i64 first-use guard global beside `object_name`.
	string LocalStaticGuard(const string& object_name);
	// PA15: a lowered function registered an automatic-object cleanup;
	// the unwind runtime declares are emitted once.
	void RequireEhRuntime();
	// Whether a direct callee may unwind (drives eh_try placement).
	bool CalleeMayUnwind(const SemNode& callee);
	// PA25: creates the operator() entries of closure-class spec
	// arguments (emission-order parity with the reference).
	void TouchClosureOperators(const FunctionSpecialization& spec);
	// 3.2p3: a constructor selected for an elided copy/move is still
	// odr-used; user-provided definitions reached through synthesized
	// bodies must be emitted even though the call itself is dropped.
	void DemandElidedCtor(const SemNode& callee);
	// PA20: the same rule for the dropped initialization actions of an
	// image-backed constant definition.
	void DemandImageInitCallees(const SemNode& item);
	// The analyzed in-class actions behind an initializer-less storage
	// definition (9.4.2p3), or null.
	const SemNode* ImageInitActions(const SemNode* node) const;
	// Handles an image-backed definition whose image renders (demands
	// the odr-used constructors, drops the actions); false when the
	// definition must initialize dynamically.
	bool TryImageBackedInit(LowGlobalInfo& info);
	// PA18: a trivial copy/move lowered as a raw object copy still
	// demands the synthesized weak definition when sema built one (a
	// user-defaulted member odr-used inside an instantiated body).
	void DemandTrivialCtorBody(const SemNode& callee);
	// PA17: the "@name" spelling of a polymorphic class's vtable
	// (demand-marks it for emission; lower_vtable.cpp).
	string VTableRef(const ClassInfo* cls);
	// PA25: the "@name" of the RTTI record of an arbitrary typeid
	// operand type (classes route through RttiRef; fundamentals,
	// pointers and incomplete class specializations render
	// encoding-keyed records).
	string RttiTypeRef(const TypePtr& type);
	enum ERttiVtableKind
	{
		RTTI_VT_CLASS,
		RTTI_VT_SI_CLASS,
		RTTI_VT_FUNDAMENTAL,
		RTTI_VT_POINTER,
		RTTI_VT_VMI_CLASS,
		RTTI_VT_FUNCTION,
		RTTI_VT_ENUM,
		RTTI_VT_KIND_COUNT
	};
	// PA25: the "@name" of an external runtime helper, declared on
	// first use with the given signature/metadata suffix.
	string ExternalRuntimeFnRef(const string& object_name,
	                            const string& declare_suffix);
	// PA33: the "@name" of the noexcept-terminate trampoline
	// (__cxa_begin_catch + std::terminate), synthesized on first use.
	string TerminateHelperRef();
	// PA25 15.1: the RTTI record a throw/catch type matches by - the
	// strong external record for fundamentals (with the weak record
	// rendered beside it), the program's own record otherwise.
	string ThrowRttiRef(const TypePtr& type);
	// PA26 dynamic_cast<void*>: the C++ library's void type_info
	// record (_ZTIv); the runtime-call presentation block names it.
	string ExternalVoidRttiRef();
	// PA25 15.1: the weak zero-filled exception-object scaffolding
	// global of a thrown type.
	void EhObjGlobal(const TypePtr& type);
	// PA25 5.2.8: whether `entity` is a unit's recognized
	// std::type_info class (the sema-owned fact behind the
	// operator==/!= address-identity fold).
	bool IsStdTypeInfo(const NamedTypeInfo* entity) const
	{
		return entity && std_type_info_entities_.count(entity);
	}
	// PA18 reference-parity fold (lower_expr.cpp BranchOnValue): a
	// branch on this namespace-scope pointer-to-function object may
	// spell the object's address only when that is provably
	// truth-equivalent to the stored value - single unit, dynamic init
	// stores a named entity's address, no write/alias of the object
	// anywhere, and no call runs before the init store.
	bool BranchSpellsFnPointerAddress(const Scope* scope,
	                                  const string& name);
	// The class record of `entity` across the added units (PA24: the
	// closure-construction lowering reads sema's field offsets).
	const ClassInfo* ProgramClass(const NamedTypeInfo* entity) const;
	// The class record behind a this-adjusted member function type.
	const ClassInfo* MethodClass(const TypePtr& adjusted) const;
	// PA32: whether a constructor callee is the fully implicit
	// trivial default constructor of its class (no user constructors,
	// no construction work in the subobject tree).
	bool TrivialDefaultConstruction(const SemNode& callee) const;
	// --- PA27 multi-vtable references (lower_vtable.cpp) ---
	// Demands the class's full vtable group (primary, views, and the
	// VTT with its construction vtables when the class is a
	// polymorphic virtual-base one).
	void VTableGroupRef(const ClassInfo* cls);
	// The "@name" of one secondary view's global; demands the group.
	string ViewVTableRef(const ClassInfo* cls, size_t view_index);
	// The address point (bytes) of a class's primary vtable / of a
	// view inside `cls`'s group.
	static unsigned long long VTableAddressPoint(const ClassInfo* cls);
	static unsigned long long ViewAddressPoint(const ClassInfo* cls);
	// PA29: the address point of one view whose header may follow the
	// view class's own (shorter) vbase table.
	static unsigned long long ViewGroupAddressPoint(
		const ClassInfo& cls, const struct ClassView& view);
	// The "@name" of the class's VTT; demands the group.
	string VttRef(const ClassInfo* cls);
	// The VTT-shape size of a class and the entry index of a direct
	// base's construction sub-block / of an own view entry.
	static size_t VttShapeSize(const ClassInfo& cls);
	static size_t SubVttIndex(const ClassInfo& derived,
	                          const ClassInfo& base);
	static size_t OwnViewVttIndex(const ClassInfo& cls, size_t ordinal);

private:
	void CollectItem(const SemNode& item);
	void ApplyDeferredDefinition(const SemNode& item,
	                             LowFunctionInfo& info);
	void DemandTreeCallees(const SemNode& node);
	void RegisterFunction(const SemNode& item, bool defined);
	void RegisterDeferred(const SemNode& item);
	LowGlobalInfo& GlobalEntry(const Scope* scope, const string& name);
	LowFunctionInfo& FunctionEntry(const Scope* scope, const string& name,
	                               const TypePtr& type,
	                               const FunctionSpecialization* spec = 0);
	LowFunctionInfo& MemberFunctionEntry(const Scope* scope,
	                                     const string& name,
	                                     const TypePtr& type,
	                                     const string& special_code);
	// The variant-independent definition key of a member function.
	string MemberDefinitionKey(const Scope* scope, const string& name,
	                           const TypePtr& type,
	                           ESpecialFunction special) const;
	void DemandFunction(LowFunctionInfo& info);
	void LowerUsedFunctions();
	void BuildLifetimeHelpers();
	// The init-order anchor of an image-backed call-initialized weak
	// object (PA23 variable templates).
	void AppendImageInitTouch(const LowGlobalInfo& info, bool is_class,
	                          SemNode& init_def);
	// Registers and lowers one @__cppgm_init / @__cppgm_fini body.
	void RegisterLifetimeHelper(const char* name, const char* role,
	                            SemNodePtr def);
	void AppendTlsWrapperDeclares(vector<string>& declares);
	// A thread-local object's first-use guard global and internal
	// `__tls_init` function built from its construction actions.
	void BuildTlsGuardedInit(size_t global_index,
	                         vector<SemNodePtr>& actions);

	// --- PA17 vtable/RTTI emission (lower_vtable.cpp) ---
	LowVTableInfo& VTableEntry(const ClassInfo* cls);
	// A this-adjusting entry thunk toward `target` ("@name"); a
	// non-zero `ret_adjust` makes it a covariant (_ZTc) thunk that
	// also shifts the returned pointer.
	string VTableThunkRef(LowFunctionInfo& target, long long adjust,
	                      const TypePtr& adjusted,
	                      long long ret_adjust = 0);
	// One slot list rendered into vtable items (`view` null for the
	// primary list; `level` resolves overriders for construction
	// vtables).
	string RenderVTableSlots(const ClassInfo& derived,
	                         const ClassView* view,
	                         const ClassInfo* level,
	                         unsigned long long level_offset,
	                         unsigned long long position);
	string RenderVTableHeader(const ClassInfo& outer,
	                          const ClassInfo& table_of,
	                          unsigned long long position,
	                          const string& rtti);
	string RenderConstructionGroup(LowVTableInfo& entry);
	void CollectConstructionBases(
		const ClassInfo& cls, unsigned long long offset,
		vector<std::pair<const ClassInfo*, unsigned long long>>& out);
	void AppendVttEntries(
		const ClassInfo& cls, unsigned long long offset,
		const map<std::pair<const ClassInfo*, unsigned long long>,
		          vector<string>>& names,
		string& items);
	static unsigned long long VBasePositionIn(const ClassInfo& outer,
	                                          const ClassInfo* base);
	// Renders every demanded-but-unrendered vtable (demanding its slot
	// functions and RTTI chain); returns whether any rendered, so the
	// caller can rerun the function-lowering sweep to a fixpoint.
	bool RenderPendingVTables();
	// Merges the polymorphic declares/globals into the output phases.
	void AppendPolymorphicSections(vector<string>* sections);
	string RenderVTableDefinition(LowVTableInfo& entry);
	// The "@name" of a class's RTTI record, rendering it (and its
	// typeinfo-name data and base chain) on first use.
	string RttiRef(const ClassInfo* cls);
	// The "@name" of the external abi typeinfo-vtable of that kind.
	string ExternalRttiVtableRef(ERttiVtableKind kind);
	void EnsurePureVirtualDeclare(const TypePtr& adjusted);
	void AppendDynamicInit(LowGlobalInfo& info, const SemNode& child,
	                       bool ref, SemNode& init_def);
	void LowerHelper(LowFunctionInfo& info, const SemNode& definition);
	string UniqueSymbol(const string& base);
	string RenderGlobal(LowGlobalInfo& info);
	string RenderScalarInit(const LowGlobalInfo& info);
	string RenderArrayItems(const LowGlobalInfo& info);
	// --- PA20 evaluated-image emission ---
	// Whether the definition's initial value comes from its evaluated
	// constant image (weak static-member definitions and
	// initializer-less storage definitions).
	bool ImageBacked(const LowGlobalInfo& info) const;
	// Attempts (once) to render the image into info.image_text; false
	// means the ordinary zero/dynamic-init paths must initialize the
	// object. BuildLifetimeHelpers asks before dropping init actions.
	bool EnsureImageText(LowGlobalInfo& info);
	// Renders the flattened typed items of the image into `out`; false
	// when a value form cannot render (engine-internal pointers,
	// bit-fields), letting the zero/dynamic paths take over.
	bool TryRenderImageItems(const struct ConstObject& image,
	                         const TypePtr& type,
	                         unsigned long long offset,
	                         unsigned long long& covered, string& out,
	                         bool in_class = false);
	bool AppendImageScalar(const struct ConstObject& image,
	                       const TypePtr& type,
	                       unsigned long long offset,
	                       unsigned long long& covered, string& out,
	                       bool in_class);
	string RenderConstItem(const struct LowerConst& value,
	                       const TypePtr& type, bool& is_zero_item);
	string RenderAddress(const struct LowerConst& value);
	string GlobalMetadata(const LowGlobalInfo& info) const;
	string RenderFunctionDeclare(const LowFunctionInfo& info);

	// deques: lowering one entity can register more (demand-driven
	// declares, string literals), so references handed out by the
	// entry accessors must survive growth.
	deque<LowGlobalInfo> globals_;
	deque<LowFunctionInfo> functions_;
	deque<LowStringLiteral> strings_;
	map<string, size_t> global_index_;    // qualified key -> index
	map<string, size_t> function_index_;  // qualified key + signature
	map<string, size_t> string_index_;    // element|bytes -> index
	set<string> symbols_;                 // taken top-level names
	bool has_main_;
	bool separate_compilation_ = false;
	int optimize_level_ = 0;
	// The added units, for whole-program scans (branch-fold analysis).
	vector<const SemUnit*> units_;
	// PA25 5.2.8: each unit's sema-recognized std::type_info entity.
	set<const NamedTypeInfo*> std_type_info_entities_;
	map<string, bool> branch_folds_;  // qualified key -> cached verdict
	// PA15: demand-emitted member/friend/special definitions, keyed by
	// MemberDefinitionKey; lifetime helper state.
	map<string, const SemNode*> member_defs_;
	bool needs_eh_runtime_;
	// The lowest functions_ index whose demand flipped since the body
	// sweep last passed it (rescans restart there, not from zero).
	size_t lower_floor_;
	// The function whose body is currently lowering (null between
	// bodies): demands fired from inside it can consult their origin
	// (the constructor-entry pairing rule keys on it).
	const LowFunctionInfo* lowering_context_ = 0;
	vector<SemNodePtr> helper_defs_;  // synthesized init/fini trees
	// Synthesized definitions whose callees were already demand-walked
	// (DemandTreeCallees); demand is monotonic, so once is enough.
	set<const SemNode*> demanded_trees_;

	// --- PA17 polymorphic emission state (lower_vtable.cpp) ---
	deque<LowVTableInfo> vtables_;
	map<const ClassInfo*, size_t> vtable_index_;
	// PA27: view-global names by (class, view index), thunk names by
	// (target symbol, adjustment), and the rendered thunk bodies
	// (appended after the ordinary function bodies).
	map<std::pair<const ClassInfo*, size_t>, string> view_names_;
	map<std::pair<string, long long>, string> thunk_names_;
	vector<string> thunk_texts_;
	// PA29: virtual member-pointer forwarding thunks by (class, slot).
	map<std::pair<const ClassInfo*, size_t>, string> pm_thunk_names_;
	map<const ClassInfo*, string> rtti_names_;   // rendered RTTI records
	vector<string> poly_declare_globals_;  // external abi/vtable declares
	vector<string> poly_globals_;          // RTTI + typeinfo-name texts
	string pure_virtual_name_;     // "" until a pure slot renders
	string pure_virtual_declare_;
	// PA25: encoding-keyed RTTI records (fundamentals, pointers,
	// incomplete or non-simple template classes) and the external
	// __cxxabiv1 vtable declares, by ERttiVtableKind.
	map<string, string> rtti_type_names_;
	string external_rtti_vtable_names_[RTTI_VT_KIND_COUNT];
	// PA25: external runtime helper declares (__cxa_bad_typeid,
	// __dynamic_cast, ...), keyed by their object name.
	map<string, string> runtime_fn_names_;
	vector<string> runtime_declares_;
	// PA33: the noexcept-terminate trampoline ("" until first demand).
	string terminate_helper_name_;
	// PA25: strong external RTTI declares (throw/catch fundamentals)
	// and the rendered exception-object scaffolding globals.
	map<string, string> external_rtti_names_;
	set<string> ehobj_rendered_;
	// PA25: instantiated bodies pending their first demand (their
	// entries appear in demand order, not registration order).
	map<string, const SemNode*> pending_fn_defs_;
};
