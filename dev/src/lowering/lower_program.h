#pragma once

#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

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
		  internal(false), definition(0)
	{}

	const Scope* scope;  // declaring namespace scope
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
	const SemNode* definition;
	string body_text;    // lowered definition text
};

struct LowGlobalInfo
{
	LowGlobalInfo()
		: type(), defined(false), used(false), internal(false),
		  is_thread_local(false), c_linkage(false), node(0)
	{}

	TypePtr type;
	string low_name;
	string object_name;
	bool defined;
	bool used;
	bool internal;
	bool is_thread_local;
	bool c_linkage;
	const SemNode* node;  // defining SN_VARIABLE (init children)
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
	// The "@__strlit__N" object of a string-literal node.
	string StringLiteralRef(const SemNode& node);
	// Registers a namespace-scope object declaration (also used for
	// block-scope extern declarations naming the global entity).
	void RegisterGlobal(const SemNode& item);

private:
	void CollectItem(const SemNode& item);
	void RegisterFunction(const SemNode& item, bool defined);
	LowGlobalInfo& GlobalEntry(const Scope* scope, const string& name);
	LowFunctionInfo& FunctionEntry(const Scope* scope, const string& name,
	                               const TypePtr& type);
	string UniqueSymbol(const string& base);
	string RenderGlobal(const LowGlobalInfo& info);
	string RenderScalarInit(const LowGlobalInfo& info);
	string RenderArrayItems(const LowGlobalInfo& info);
	string RenderConstItem(const struct LowerConst& value,
	                       const TypePtr& type, bool& is_zero_item);
	string RenderAddress(const struct LowerConst& value);
	string GlobalMetadata(const LowGlobalInfo& info) const;
	string RenderFunctionDeclare(const LowFunctionInfo& info);

	vector<LowGlobalInfo> globals_;
	vector<LowFunctionInfo> functions_;
	vector<LowStringLiteral> strings_;
	map<string, size_t> global_index_;    // qualified key -> index
	map<string, size_t> function_index_;  // qualified key + signature
	map<string, size_t> string_index_;    // element|bytes -> index
	set<string> symbols_;                 // taken top-level names
	bool has_main_;
};
