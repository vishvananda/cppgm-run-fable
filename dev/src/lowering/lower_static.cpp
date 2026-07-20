#include "lowering/lower_function.h"

#include "lowering/lower_const.h"

#include <cstdio>
#include <stdexcept>

#include "lowering/lower_name.h"
#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// PA20 function-local statics: hoisted storage, the first-use guard,
// and declaration-point initialization; PA36 adds host-mode
// destructor registration (__cxa_atexit / __cxa_thread_atexit) and
// the guard's weak/thread-local mirroring of its object.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

}  // namespace

// PA20 function-local statics: the object hoists to an internal
// global named from the declarator's token span. A non-class scalar
// with a constant initializer keeps its static image; every other
// form initializes at the declaration point behind an i64 first-use
// guard (the reference shape).
void FunctionLowerer::LowerLocalStatic(const SemNode& node)
{
	// Host mode registers the destructor with the CRT at first-use
	// time (__cxa_atexit / __cxa_thread_atexit, 3.6.3p1/p2);
	// whole-program mode keeps its pinned dtor-less shapes.
	if (node.needs_dtor &&
	    (!program_.SeparateCompilation() ||
	     RemoveTopCv(node.type)->kind == TK_ARRAY))
		throw OutsideBoundary("function-local static with a destructor");
	// A weak (vague-linkage) enclosing definition names its statics
	// from the function's mangled symbol so every translation unit's
	// copy merges onto one weak object; internal-emission functions
	// key by their local symbol.
	string owner_part = info_.low_name;
	if (info_.weak && !info_.object_name.empty())
	{
		static const char* digits = "0123456789abcdef";
		owner_part = "function_symbol_";
		for (size_t i = 0; i < info_.object_name.size(); i++)
		{
			unsigned char c = info_.object_name[i];
			owner_part += digits[c >> 4];
			owner_part += digits[c & 15];
		}
	}
	string base_name = "__local_static__" + owner_part + "__" +
		node.entity_name + "__tokens" +
		to_string(node.decl_begin_token) + "_" +
		to_string(node.decl_end_token);
	LowGlobalInfo& info = program_.RegisterLocalStatic(
		node, base_name, info_.weak && !info_.object_name.empty());
	const TypePtr& declared = node.type;
	TypePtr inner = declared;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	bool needs_guard = IsReferenceType(declared) ||
		declared->kind == TK_ARRAY ||
		RemoveTopCv(inner)->kind == TK_CLASS;
	if (!needs_guard)
	{
		LowerConst constant;
		if (node.children.empty() ||
		    EvaluateLowerConst(*node.children[0], constant))
			return;
	}
	// Scalars and references zero-fill statically and store at first
	// use; arrays and class objects keep whatever image renders and
	// re-run their initialization actions behind the guard.
	if (declared->kind != TK_ARRAY &&
	    RemoveTopCv(inner)->kind != TK_CLASS)
		info.dynamic_init = true;
	bool host_shared = program_.SeparateCompilation() &&
		info_.weak && !info_.object_name.empty();
	string guard_ref = "@" + program_.LocalStaticGuard(
		info.low_name, host_shared, info.is_thread_local);
	string ready = NewLabel("local_static_ready");
	string init_label = NewLabel("local_static_init");
	string flag = NewTemp();
	Emit(flag + " = load i64 " + guard_ref);
	string test = NewTemp();
	Emit(test + " = cmp ne i64 " + flag + ", 0");
	ReferenceLabel(ready);
	ReferenceLabel(init_label);
	Terminate("branch " + test + ", ^" + ready + ", ^" + init_label);
	OpenBlock(init_label);
	string base = NewTemp();
	Emit(base + " = addr @" + info.low_name);
	LowerLocalStaticInit(node, base);
	if (node.needs_dtor)
		EmitLocalStaticDtorRegistration(node, base);
	Emit("store i64 1, " + guard_ref);
	ReferenceLabel(ready);
	Terminate("jump ^" + ready);
	OpenBlock(ready);
}

// 3.6.3p1/p2 host mode: the completed object's destructor registers
// with the CRT inside the first-use guard - after construction (a
// throwing constructor registers nothing), through
// __cxa_thread_atexit for thread storage duration.
void FunctionLowerer::EmitLocalStaticDtorRegistration(const SemNode& node,
                                                      const string& base)
{
	const SemNode* dtor_callee = 0;
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		if (child.kind == SN_DESTRUCTOR_ACTION &&
		    !child.children.empty() &&
		    !child.children[0]->children.empty())
			dtor_callee = child.children[0]->children[0].get();
	}
	if (!dtor_callee)
		throw runtime_error("local static lost its destructor action");
	string dtor = NewTemp();
	Emit(dtor + " = addr " + program_.MemberFunctionRef(*dtor_callee));
	string dso = NewTemp();
	Emit(dso + " = addr " + program_.DsoHandleRef());
	const char* hook = node.is_thread_local_decl
		? "__cxa_thread_atexit" : "__cxa_atexit";
	string registrar = program_.ExternalRuntimeFnRef(
		hook,
		string("(%arg0 : ptr, %arg1 : ptr, %arg2 : ptr) -> i32 "
		       "[linkage=c, binding=strong, object=") + hook + "]");
	string result = NewTemp();
	Emit(result + " = call i32 " + registrar + "(" + dtor + ", " +
	     base + ", " + dso + ")");
}

void FunctionLowerer::LowerLocalStaticInit(const SemNode& node,
                                           const string& base)
{
	const TypePtr& declared = node.type;
	if (IsReferenceType(declared))
	{
		string address = LowerAddressExpr(*node.children[0]);
		Emit("store ptr " + address + ", " + base);
		return;
	}
	TypePtr bare = RemoveTopCv(declared);
	if (bare->kind == TK_ARRAY && !node.children.empty() &&
	    node.children[0]->kind == SN_BRACED_INIT_LIST)
	{
		// Flat element stores at immediate byte offsets off the base.
		const SemNode& braced = *node.children[0];
		TypePtr element = RemoveTopCv(bare->target);
		unsigned long long stride = TypeSize(element);
		for (size_t i = 0; i < braced.children.size(); i++)
		{
			string target = base;
			if (i > 0)
			{
				target = NewTemp();
				Emit(target + " = index i8 " + base + ", " +
				     to_string(i * stride));
			}
			string value = LowerValueAs(*braced.children[i], element,
			                            LCC_INIT);
			Emit("store " + LowerValueType(element) + " " + value +
			     ", " + target);
		}
		return;
	}
	// Class objects and class arrays: constructor calls address the
	// base at immediate element offsets; member-wise aggregate stores
	// lower as ordinary statements.
	unsigned long long stride = bare->kind == TK_ARRAY
		? TypeSize(RemoveTopCv(bare->target)) : 0;
	size_t element = 0;
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		if (child.kind == SN_CONSTRUCTOR_ACTION)
		{
			unsigned long long offset = element * stride;
			element++;
			if (child.trivial_init || child.elided)
				continue;
			string target = base;
			if (offset)
			{
				target = NewTemp();
				Emit(target + " = index i8 " + base + ", " +
				     to_string(offset));
			}
			bool saved = in_lifetime_action_;
			in_lifetime_action_ = true;
			LowerConstructorCall(child, target);
			in_lifetime_action_ = saved;
			continue;
		}
		if (child.kind == SN_EXPRESSION_STATEMENT)
		{
			LowerStatement(child);
			continue;
		}
		if (child.kind == SN_DESTRUCTOR_ACTION)
			continue;
		// The scalar initializer value.
		TypePtr value_type = RemoveTopCv(declared);
		string value = LowerValueAs(child, value_type, LCC_INIT);
		Emit("store " + LowerValueType(value_type) + " " + value +
		     ", " + base);
	}
}

// The i64 first-use guard beside a dynamically initialized
// function-local static; returns its symbol. A weak object's guard
// merges weak beside it (every unit's copy shares one first-use
// fact), and a thread-local object's guard is thread-local (first
// control passage is per thread, 6.7p4).
string LowerProgram::LocalStaticGuard(const string& object_name,
                                      bool weak,
                                      bool thread_local_storage)
{
	string key = "#local_static_guard#" + object_name;
	map<string, size_t>::iterator found = global_index_.find(key);
	if (found != global_index_.end())
		return globals_[found->second].low_name;
	LowGlobalInfo guard;
	guard.type = MakeFundamentalType(FT_LONG_INT);
	guard.low_name = UniqueSymbol(object_name + "__guard");
	guard.internal = !weak;
	guard.weak = weak;
	if (weak)
		guard.object_name = "@" + guard.low_name;
	guard.is_thread_local = thread_local_storage;
	guard.defined = true;
	global_index_[key] = globals_.size();
	globals_.push_back(guard);
	return globals_.back().low_name;
}

// PA36: __cxa_atexit / __cxa_thread_atexit registrations pass the
// image's __dso_handle (the host CRT defines it per executable or
// shared object).
string LowerProgram::DsoHandleRef()
{
	if (!dso_handle_name_.empty())
		return "@" + dso_handle_name_;
	dso_handle_name_ = UniqueSymbol("__external_dso_handle");
	poly_declare_globals_.push_back(
		"declare global @" + dso_handle_name_ +
		" [binding=strong, object=__dso_handle]");
	return "@" + dso_handle_name_;
}

// PA20: the per-scope identity of a function-local static (also
// spelled in lower_unit.cpp beside the other global-entry keys).
static string LocalStaticIdentity(const Scope* scope, const string& name)
{
	char suffix[32];
	std::snprintf(suffix, sizeof(suffix), "#%p", (const void*)scope);
	return LowerScopeKey(scope) + name + suffix;
}

// PA20: whether a (block-scope) entity resolved to a hoisted
// function-local static.
bool LowerProgram::HasGlobal(const Scope* scope, const string& name) const
{
	return global_index_.count(LocalStaticIdentity(scope, name)) > 0;
}

// PA20: hoists a function-local static object to an internal global
// under its per-scope key, named from the declarator's token span
// (the reference presentation).
LowGlobalInfo& LowerProgram::RegisterLocalStatic(const SemNode& item,
                                                 const string& base_name,
                                                 bool weak)
{
	string key = LocalStaticIdentity(item.entity_scope,
	                                 item.entity_name);
	map<string, size_t>::iterator found = global_index_.find(key);
	if (found != global_index_.end())
		return globals_[found->second];
	LowGlobalInfo info;
	info.defined = true;
	info.internal = !weak;
	info.weak = weak;
	info.node = &item;
	info.type = item.type;
	// Host mode gives block-scope thread_local objects real TLS
	// storage (and the wrapper the tls_addr routing expects);
	// whole-program mode keeps the single-threaded direct model.
	info.is_thread_local =
		item.is_thread_local_decl && separate_compilation_;
	info.low_name = UniqueSymbol(base_name);
	// The weak copies merge by the symbol itself (no Itanium _ZZ...E
	// spelling in the LowIR contract).
	if (weak)
		info.object_name = "@" + info.low_name;
	global_index_[key] = globals_.size();
	globals_.push_back(info);
	return globals_.back();
}
