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
	// carry the hidden `this` first parameter.
	bool weak;
	bool is_method;
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
	// PA20: a hidden-friend definition (deferred, namespace scope):
	// its references count as semantic demand, so its body lowers even
	// unused. Weak constexpr/inline namespace functions do not.
	bool friend_def = false;
	const SemNode* definition;
	size_t index;        // position in functions_ (demand rescan key)
	string body_text;    // lowered definition text
};

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
};

class LowerProgram
{
public:
	LowerProgram();

	// Registers one bound translation unit's namespace-scope items.
	void AddUnit(const SemUnit& unit);

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
	// The "@__strlit__N" object of a string-literal node.
	string StringLiteralRef(const SemNode& node);
	// Registers a namespace-scope object declaration (also used for
	// block-scope extern declarations naming the global entity).
	void RegisterGlobal(const SemNode& item);
	// --- PA20 function-local statics ---
	// Whether (scope, name) already resolves to a registered global.
	bool HasGlobal(const Scope* scope, const string& name) const;
	// Hoists a local static to an internal global under its block
	// -scope entity key.
	LowGlobalInfo& RegisterLocalStatic(const SemNode& item,
	                                   const string& base_name);
	// The i64 first-use guard global beside `object_name`.
	string LocalStaticGuard(const string& object_name);
	// PA15: a lowered function registered an automatic-object cleanup;
	// the unwind runtime declares are emitted once.
	void RequireEhRuntime();
	// Whether a direct callee may unwind (drives eh_try placement).
	bool CalleeMayUnwind(const SemNode& callee);
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
	// PA18 reference-parity fold (lower_expr.cpp BranchOnValue): a
	// branch on this namespace-scope pointer-to-function object may
	// spell the object's address only when that is provably
	// truth-equivalent to the stored value - single unit, dynamic init
	// stores a named entity's address, no write/alias of the object
	// anywhere, and no call runs before the init store.
	bool BranchSpellsFnPointerAddress(const Scope* scope,
	                                  const string& name);

private:
	void CollectItem(const SemNode& item);
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
	void AppendTlsWrapperDeclares(vector<string>& declares);
	// A thread-local object's first-use guard global and internal
	// `__tls_init` function built from its construction actions.
	void BuildTlsGuardedInit(size_t global_index,
	                         vector<SemNodePtr>& actions);

	// --- PA17 vtable/RTTI emission (lower_vtable.cpp) ---
	// The class record behind a this-adjusted member function type.
	const ClassInfo* MethodClass(const TypePtr& adjusted) const;
	LowVTableInfo& VTableEntry(const ClassInfo* cls);
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
	// The "@name" of the external abi class/si_class typeinfo-vtable.
	string ExternalRttiVtableRef(bool si);
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
	                         unsigned long long& covered, string& out);
	bool AppendImageScalar(const struct ConstObject& image,
	                       const TypePtr& type,
	                       unsigned long long offset,
	                       unsigned long long& covered, string& out);
	// The class record of `entity` across the added units.
	const ClassInfo* ProgramClass(const NamedTypeInfo* entity) const;
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
	// The added units, for whole-program scans (branch-fold analysis).
	vector<const SemUnit*> units_;
	map<string, bool> branch_folds_;  // qualified key -> cached verdict
	// PA15: demand-emitted member/friend/special definitions, keyed by
	// MemberDefinitionKey; lifetime helper state.
	map<string, const SemNode*> member_defs_;
	bool needs_eh_runtime_;
	// The lowest functions_ index whose demand flipped since the body
	// sweep last passed it (rescans restart there, not from zero).
	size_t lower_floor_;
	vector<SemNodePtr> helper_defs_;  // synthesized init/fini trees
	// Synthesized definitions whose callees were already demand-walked
	// (DemandTreeCallees); demand is monotonic, so once is enough.
	set<const SemNode*> demanded_trees_;

	// --- PA17 polymorphic emission state (lower_vtable.cpp) ---
	deque<LowVTableInfo> vtables_;
	map<const ClassInfo*, size_t> vtable_index_;
	map<const ClassInfo*, string> rtti_names_;   // rendered RTTI records
	vector<string> poly_declare_globals_;  // external abi/vtable declares
	vector<string> poly_globals_;          // RTTI + typeinfo-name texts
	string pure_virtual_name_;     // "" until a pure slot renders
	string pure_virtual_declare_;
	string external_class_rtti_name_;
	string external_si_rtti_name_;
};
