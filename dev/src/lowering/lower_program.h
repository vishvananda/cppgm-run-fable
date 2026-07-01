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
	                   const TypePtr& type);
	// The "@name" spelling of a method / constructor / destructor
	// callee node (PA15: demand-marks the weak definition).
	string MemberFunctionRef(const SemNode& callee);
	// The "@__strlit__N" object of a string-literal node.
	string StringLiteralRef(const SemNode& node);
	// Registers a namespace-scope object declaration (also used for
	// block-scope extern declarations naming the global entity).
	void RegisterGlobal(const SemNode& item);
	// PA15: a lowered function registered an automatic-object cleanup;
	// the unwind runtime declares are emitted once.
	void RequireEhRuntime();
	// Whether a direct callee may unwind (drives eh_try placement).
	bool CalleeMayUnwind(const SemNode& callee);
	// 3.2p3: a constructor selected for an elided copy/move is still
	// odr-used; user-provided definitions reached through synthesized
	// bodies must be emitted even though the call itself is dropped.
	void DemandElidedCtor(const SemNode& callee);

private:
	void CollectItem(const SemNode& item);
	void DemandTreeCallees(const SemNode& node);
	void RegisterFunction(const SemNode& item, bool defined);
	void RegisterDeferred(const SemNode& item);
	LowGlobalInfo& GlobalEntry(const Scope* scope, const string& name);
	LowFunctionInfo& FunctionEntry(const Scope* scope, const string& name,
	                               const TypePtr& type);
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
	void AppendDynamicInit(LowGlobalInfo& info, const SemNode& child,
	                       bool ref, SemNode& init_def);
	void LowerHelper(LowFunctionInfo& info, const SemNode& definition);
	string UniqueSymbol(const string& base);
	string RenderGlobal(const LowGlobalInfo& info);
	string RenderScalarInit(const LowGlobalInfo& info);
	string RenderArrayItems(const LowGlobalInfo& info);
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
	// PA15: demand-emitted member/friend/special definitions, keyed by
	// MemberDefinitionKey; lifetime helper state.
	map<string, const SemNode*> member_defs_;
	bool needs_eh_runtime_;
	// The lowest functions_ index whose demand flipped since the body
	// sweep last passed it (rescans restart there, not from zero).
	size_t lower_floor_;
	vector<SemNodePtr> helper_defs_;  // synthesized init/fini trees
};
