#include "lowering/lower_program.h"

#include <cstring>
#include <stdexcept>

#include "lowering/lower_const.h"
#include "lowering/lower_function.h"
#include "lowering/lower_name.h"
#include "lowering/lower_types.h"
#include "sema/const_eval.h"
#include "sema/const_expr.h"

using std::runtime_error;
using std::to_string;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

// The program-wide identity key of a namespace-scope entity.
string QualifiedKey(const Scope* scope, const string& name)
{
	return LowerScopeKey(scope) + name;
}

// PA20: the per-scope identity of a function-local static (the plain
// key collapses block scopes so block-scope extern declarations reach
// their namespace entities; hoisted statics must stay distinct).
string LocalStaticKey(const Scope* scope, const string& name)
{
	char suffix[32];
	std::snprintf(suffix, sizeof(suffix), "#%p", (const void*)scope);
	return QualifiedKey(scope, name) + suffix;
}

}  // namespace

LowerProgram::LowerProgram()
	: has_main_(false), needs_eh_runtime_(false), lower_floor_(0)
{
	symbols_.insert("main");
}

// The lowering entry type of a function-template specialization:
// non-static member specializations key on the this-adjusted form
// their instantiated body items carry.
static TypePtr MemberSpecEntryType(const FunctionSpecialization& spec)
{
	const TemplateInfo* tmpl = spec.owner;
	if (!tmpl || !tmpl->member_of || tmpl->member_static)
		return spec.type;
	TypePtr class_type = MakeNamedType(TK_CLASS, tmpl->member_of);
	class_type = MakeCvQualifiedType(class_type, spec.type->is_const,
	                                 spec.type->is_volatile);
	vector<TypePtr> parameters;
	parameters.push_back(MakePointerType(class_type, false, false));
	for (size_t i = 0; i < spec.type->parameters.size(); i++)
		parameters.push_back(spec.type->parameters[i]);
	TypePtr adjusted = MakeFunctionType(spec.type->target, parameters,
	                                    spec.type->variadic);
	if (spec.type->ref_qual)
		adjusted = MakeRefQualifiedType(adjusted, spec.type->ref_qual);
	return adjusted;
}

void LowerProgram::AddUnit(const SemUnit& unit)
{
	units_.push_back(&unit);
	if (unit.std_type_info)
		std_type_info_entities_.insert(unit.std_type_info);
	for (size_t i = 0; i < unit.items.size(); i++)
		CollectItem(*unit.items[i]);
	// unit.synthesized is the PA12 dump artifact; the real demand-driven
	// member definitions arrive through unit.deferred.
	for (size_t i = 0; i < unit.deferred.size(); i++)
		RegisterDeferred(*unit.deferred[i]);
	// PA18 14.7.2p8: an explicit instantiation definition emits every
	// member definition its class had instantiated by that point
	// unconditionally (later-registered definitions stay on-demand,
	// 14.7.2p9).
	for (size_t e = 0; e < unit.explicit_instantiations.size(); e++)
	{
		const NamedTypeInfo* entity =
			unit.explicit_instantiations[e].entity;
		size_t deferred_end = unit.explicit_instantiations[e].deferred_end;
		for (size_t i = 0; i < deferred_end && i < unit.deferred.size();
		     i++)
		{
			const SemNode& item = *unit.deferred[i];
			if (item.kind != SN_FUNCTION_DEFINITION ||
			    !item.entity_scope || item.synthesized ||
			    item.entity_scope->entity != entity)
				continue;
			LowFunctionInfo* info;
			if (item.special != SF_NONE)
			{
				bool is_ctor = item.special == SF_CONSTRUCTOR ||
					item.special == SF_CONSTRUCTOR_BASE;
				info = &MemberFunctionEntry(item.entity_scope,
				                            item.entity_name, item.type,
				                            is_ctor ? "C1" : "D1");
			}
			else if (item.is_method)
				info = &MemberFunctionEntry(item.entity_scope,
				                            item.entity_name, item.type,
				                            "");
			else
				info = &FunctionEntry(item.entity_scope,
				                      item.entity_name, item.type,
				                      item.fn_spec);
			info->object_root = true;
			DemandFunction(*info);
		}
	}
	// PA21 14.7.2: an explicit-instantiation definition naming one
	// member of a specialization emits that member unconditionally.
	for (size_t e = 0; e < unit.explicit_member_instantiations.size();
	     e++)
	{
		const SemUnit::ExplicitMemberInstantiation& record =
			unit.explicit_member_instantiations[e];
		for (size_t i = 0; i < unit.deferred.size(); i++)
		{
			const SemNode& item = *unit.deferred[i];
			if (item.kind != SN_FUNCTION_DEFINITION ||
			    item.synthesized || item.special != SF_NONE ||
			    item.entity_scope != record.scope ||
			    item.entity_name != record.name)
				continue;
			LowFunctionInfo* info;
			if (item.is_method)
				info = &MemberFunctionEntry(item.entity_scope,
				                            item.entity_name,
				                            item.type, "");
			else
				info = &FunctionEntry(item.entity_scope,
				                      item.entity_name, item.type,
				                      item.fn_spec);
			info->object_root = true;
			DemandFunction(*info);
		}
	}
	// PA21 14.7.2 function forms: explicit-instantiation definitions
	// are unconditional emission roots; extern-template declarations
	// suppress the local weak body in favor of an external declare.
	for (size_t e = 0; e < unit.explicit_fn_instantiations.size(); e++)
	{
		const FunctionSpecialization* spec =
			unit.explicit_fn_instantiations[e];
		LowFunctionInfo& info = FunctionEntry(
			spec->self.owner, spec->name,
			MemberSpecEntryType(*spec), spec);
		info.object_root = true;
		info.extern_suppressed = false;
		DemandFunction(info);
	}
	for (size_t e = 0; e < unit.extern_fn_suppressions.size(); e++)
	{
		const FunctionSpecialization* spec =
			unit.extern_fn_suppressions[e];
		if (spec->inst_definition)
			continue;
		LowFunctionInfo& info = FunctionEntry(
			spec->self.owner, spec->name,
			MemberSpecEntryType(*spec), spec);
		info.extern_suppressed = true;
	}
	// PA17: a class whose key function is defined in this unit anchors
	// its vtable here (emitted strong even without local construction).
	const vector<const ClassInfo*>& classes = unit.classes.All();
	for (size_t i = 0; i < classes.size(); i++)
		if (classes[i]->is_polymorphic && classes[i]->key_defined_in_tu)
		{
			LowVTableInfo& entry = VTableEntry(classes[i]);
			entry.used = true;
			entry.strong = true;
		}
}

// A deferred in-class definition (method, hidden friend, constructor,
// destructor): registered as a weak definition, emitted on demand.
void LowerProgram::RegisterDeferred(const SemNode& item)
{
	if (item.special != SF_NONE)
	{
		string key = MemberDefinitionKey(item.entity_scope,
		                                 item.entity_name, item.type,
		                                 item.special);
		member_defs_[key] = &item;
		bool is_ctor = item.special == SF_CONSTRUCTOR ||
			item.special == SF_CONSTRUCTOR_BASE;
		if (!item.inline_def)
		{
			// An out-of-class constructor or destructor prints both
			// ABI entries.
			MemberFunctionEntry(item.entity_scope, item.entity_name,
			                    item.type,
			                    is_ctor ? "C2" : "D2").used = true;
			MemberFunctionEntry(item.entity_scope, item.entity_name,
			                    item.type,
			                    is_ctor ? "C1" : "D1").used = true;
		}
		else if (item.inline_root)
			// A spelled-inline out-of-class entry still prints, but
			// weak and folded: the complete-object entry emits and the
			// base entry rides the usual alias.
			MemberFunctionEntry(item.entity_scope, item.entity_name,
			                    item.type,
			                    is_ctor ? "C1" : "D1").used = true;
		return;
	}
	if (item.is_method)
	{
		string key = MemberDefinitionKey(item.entity_scope,
		                                 item.entity_name, item.type,
		                                 SF_NONE);
		member_defs_[key] = &item;
		return;
	}
	// PA25: an instantiated body's entry appears at its first demand
	// (the reference's emission order), not at registration.
	if (item.from_instantiation && !item.demand_strong &&
	    item.entity_scope)
	{
		string key = QualifiedKey(item.entity_scope, item.entity_name) +
			"#" + DescribeType(item.type);
		pending_fn_defs_[key] = &item;
		return;
	}
	// Static member functions and hidden friends register as ordinary
	// entries; friend definitions emit unconditionally, static members
	// on demand.
	LowFunctionInfo& info = FunctionEntry(item.entity_scope,
	                                      item.entity_name, item.type,
	                                      item.fn_spec);
	ApplyDeferredDefinition(item, info);
}

// The shared definition-attachment of a deferred namespace-scope
// item (immediate registration or first-demand creation).
void LowerProgram::ApplyDeferredDefinition(const SemNode& item,
                                           LowFunctionInfo& info)
{
	if (item.fn_spec && !info.fn_spec)
	{
		info.fn_spec = item.fn_spec;
		info.object_name = MangleFunctionTemplateObjectName(*item.fn_spec);
	}
	if (!info.defined)
	{
		info.defined = true;
		info.weak = true;
		info.demand_strong = item.demand_strong;
		info.definition = &item;
		info.unwind_no = item.unwind_no;
		// An instantiated friend body binds only when odr-used
		// (14.7.1), so it never counts as definition-time demand.
		info.friend_def = item.entity_scope &&
			item.entity_scope->kind == SCOPE_NAMESPACE &&
			!item.is_constexpr_fn && !item.from_instantiation;
		if (item.internal_fn)
		{
			// PA24: a captureless lambda's function has internal
			// linkage and no external object identity; it still emits
			// only on demand (the weak flag drives the sweep).
			info.internal = true;
			info.object_name.clear();
		}
	}
}

void LowerProgram::CollectItem(const SemNode& item)
{
	switch (item.kind)
	{
	case SN_NAMESPACE_DEFINITION:
		for (size_t i = 0; i < item.children.size(); i++)
			CollectItem(*item.children[i]);
		return;
	case SN_VARIABLE:
		RegisterGlobal(item);
		return;
	case SN_FUNCTION_DEFINITION:
		RegisterFunction(item, true);
		return;
	case SN_FUNCTION_DECLARATION:
		RegisterFunction(item, false);
		return;
	case SN_TYPE_ALIAS:
		return;
	default:
		throw OutsideBoundary("namespace-scope item");
	}
}

LowGlobalInfo& LowerProgram::GlobalEntry(const Scope* scope,
                                         const string& name)
{
	string key = QualifiedKey(scope, name);
	map<string, size_t>::iterator found = global_index_.find(key);
	if (found != global_index_.end())
		return globals_[found->second];
	const ScopeBinding* binding = FindOwnBinding(*scope, name);
	if (!binding)
		throw runtime_error("unknown namespace-scope object " + name);
	LowGlobalInfo info;
	info.type = binding->type;
	info.is_thread_local = binding->is_thread_local;
	info.low_name = UniqueSymbol(LowerScopePath(scope) +
	                             LowerSanitizeName(name));
	if (binding->var_spec_template)
		info.object_name = MangleVariableTemplateObjectName(
			scope, *binding->var_spec_template,
			binding->var_spec_args);
	else
		info.object_name = MangleVariableObjectName(scope, name);
	global_index_[key] = globals_.size();
	globals_.push_back(info);
	return globals_.back();
}

// PA20: whether a (block-scope) entity resolved to a hoisted
// function-local static.
bool LowerProgram::HasGlobal(const Scope* scope, const string& name) const
{
	return global_index_.count(LocalStaticKey(scope, name)) > 0;
}

// PA20: hoists a function-local static object to an internal global
// under its per-scope key, named from the declarator's token span
// (the reference presentation).
LowGlobalInfo& LowerProgram::RegisterLocalStatic(const SemNode& item,
                                                 const string& base_name,
                                                 bool weak)
{
	string key = LocalStaticKey(item.entity_scope, item.entity_name);
	map<string, size_t>::iterator found = global_index_.find(key);
	if (found != global_index_.end())
		return globals_[found->second];
	LowGlobalInfo info;
	info.defined = true;
	info.internal = !weak;
	info.weak = weak;
	info.node = &item;
	info.type = item.type;
	info.low_name = UniqueSymbol(base_name);
	// The weak copies merge by the symbol itself (no Itanium _ZZ...E
	// spelling in the LowIR contract).
	if (weak)
		info.object_name = "@" + info.low_name;
	global_index_[key] = globals_.size();
	globals_.push_back(info);
	return globals_.back();
}

// The i64 first-use guard beside a dynamically initialized
// function-local static; returns its symbol.
string LowerProgram::LocalStaticGuard(const string& object_name)
{
	string key = "#local_static_guard#" + object_name;
	map<string, size_t>::iterator found = global_index_.find(key);
	if (found != global_index_.end())
		return globals_[found->second].low_name;
	LowGlobalInfo guard;
	guard.type = MakeFundamentalType(FT_LONG_INT);
	guard.low_name = UniqueSymbol(object_name + "__guard");
	guard.internal = true;
	guard.defined = true;
	global_index_[key] = globals_.size();
	globals_.push_back(guard);
	return globals_.back().low_name;
}

void LowerProgram::RegisterGlobal(const SemNode& item)
{
	LowGlobalInfo& info = GlobalEntry(item.entity_scope,
	                                  item.entity_name);
	const ScopeBinding* binding = FindOwnBinding(*item.entity_scope,
	                                             item.entity_name);
	info.type = binding->type;  // redeclarations may complete bounds
	bool defines = !item.children.empty() || !item.is_extern_decl ||
		item.weak_def;
	if (defines && !info.defined)
	{
		info.defined = true;
		info.node = &item;
		// PA20: the sema-evaluated constant image of an object-valued
		// constexpr definition.
		for (size_t u = 0; u < units_.size() && !info.image; u++)
		{
			map<const SemNode*,
			    shared_ptr<const ConstObject>>::const_iterator found =
				units_[u]->const_images.find(&item);
			if (found != units_[u]->const_images.end())
				info.image = found->second;
		}
	}
	bool is_const = false;
	bool is_volatile = false;
	TopCv(info.type, is_const, is_volatile);
	if (binding->has_value && is_const)
		info.folded_const = true;
	if (item.weak_def)
		info.weak = true;
	else if (item.is_static_decl ||
	         LowerInUnnamedNamespace(item.entity_scope) ||
	         (is_const && !item.is_extern_decl))
		info.internal = true;
	if (item.is_thread_local_decl)
		info.is_thread_local = true;
	if (item.c_linkage)
	{
		info.c_linkage = true;
		info.object_name.clear();
	}
}

LowFunctionInfo& LowerProgram::FunctionEntry(const Scope* scope,
                                             const string& name,
                                             const TypePtr& type,
                                             const FunctionSpecialization* spec)
{
	string key = QualifiedKey(scope, name) + "#" + DescribeType(type);
	map<string, size_t>::iterator found = function_index_.find(key);
	if (found != function_index_.end())
	{
		LowFunctionInfo& existing = functions_[found->second];
		// PA18: a use may reach the entry before the definition item
		// carries the specialization identity; adopt it on sight.
		if (spec && !existing.fn_spec)
		{
			existing.fn_spec = spec;
			existing.object_name =
				MangleFunctionTemplateObjectName(*spec);
		}
		map<string, const SemNode*>::iterator pending =
			pending_fn_defs_.find(key);
		if (pending != pending_fn_defs_.end())
		{
			const SemNode* item = pending->second;
			pending_fn_defs_.erase(pending);
			ApplyDeferredDefinition(*item, existing);
		}
		return existing;
	}
	LowFunctionInfo info;
	info.scope = scope;
	info.name = name;
	info.type = type;
	info.fn_spec = spec;
	info.is_main = name == "main" && scope->kind == SCOPE_NAMESPACE &&
		!scope->parent;
	// The implicitly declared global allocation functions carry their
	// builtin object names (3.7.4).
	bool global_scope = scope->kind == SCOPE_NAMESPACE && !scope->parent;
	string builtin;
	if (global_scope && type->parameters.size() == 1)
	{
		if (name == "operator new" &&
		    type->parameters[0]->kind == TK_FUNDAMENTAL)
			builtin = "operator_new";
		else if (name == "operator new[]" &&
		         type->parameters[0]->kind == TK_FUNDAMENTAL)
			builtin = "operator_new__";
		else if (name == "operator delete" &&
		         type->parameters[0]->kind == TK_POINTER)
			builtin = "operator_delete";
		else if (name == "operator delete[]" &&
		         type->parameters[0]->kind == TK_POINTER)
			builtin = "operator_delete__";
	}
	if (!builtin.empty())
	{
		info.low_name = UniqueSymbol(builtin);
		info.object_name = "cppgm_builtin_" +
			(builtin == "operator_new" ? string("operator_new")
			 : builtin == "operator_new__" ? string("operator_new_array")
			 : builtin == "operator_delete" ? string("operator_delete")
			 : string("operator_delete_array"));
		info.unwind_no = builtin.compare(0, 15, "operator_delete") == 0;
		info.index = functions_.size();
		function_index_[key] = functions_.size();
		functions_.push_back(info);
		return functions_.back();
	}
	size_t overload = LowerOverloadIndex(scope, name, type);
	string base = LowerScopePath(scope) + LowerSanitizeName(name);
	if (overload)
		base += "__ov" + to_string(overload + 1);
	info.low_name = info.is_main ? "main" : UniqueSymbol(base);
	info.internal = LowerInUnnamedNamespace(scope);
	info.deleted = LowerOverloadDeleted(scope, name, type);
	if (spec)
		info.object_name = MangleFunctionTemplateObjectName(*spec);
	else if (!info.is_main)
		info.object_name = MangleFunctionObjectName(scope, name, type);
	info.index = functions_.size();
	function_index_[key] = functions_.size();
	functions_.push_back(info);
	map<string, const SemNode*>::iterator pending =
		pending_fn_defs_.find(key);
	if (pending != pending_fn_defs_.end())
	{
		const SemNode* item = pending->second;
		pending_fn_defs_.erase(pending);
		ApplyDeferredDefinition(*item, functions_.back());
	}
	return functions_.back();
}

string LowerProgram::MemberDefinitionKey(const Scope* scope,
                                         const string& name,
                                         const TypePtr& type,
                                         ESpecialFunction special) const
{
	const char* kind = "fn";
	if (special == SF_CONSTRUCTOR || special == SF_CONSTRUCTOR_BASE)
		kind = "ctor";
	else if (special == SF_DESTRUCTOR || special == SF_DESTRUCTOR_BASE)
		kind = "dtor";
	// Constructor/destructor keys drop the spelled name: the class
	// scope and kind identify them, and a specialization's definitions
	// carry the pattern spelling while call sites spell the
	// specialization ("RefPair" vs "RefPair<int>").
	string spelled = special == SF_NONE ? name : string();
	return LowerScopeKey(scope) + spelled + "#" + DescribeType(type) +
		"#" + kind;
}

// PA21: whether a declared constructor type matches a this-adjusted
// member entry type (the entry's leading object pointer dropped).
static bool CtorEntryMatches(const TypePtr& declared,
                             const TypePtr& adjusted)
{
	if (!declared || adjusted->parameters.empty() ||
	    declared->parameters.size() + 1 != adjusted->parameters.size())
		return false;
	for (size_t i = 0; i < declared->parameters.size(); i++)
		if (!TypeEquals(declared->parameters[i],
		                adjusted->parameters[i + 1]))
			return false;
	return true;
}

LowFunctionInfo& LowerProgram::MemberFunctionEntry(
	const Scope* scope, const string& name, const TypePtr& type,
	const string& special_code)
{
	string key = QualifiedKey(scope, name) + "#" + DescribeType(type) +
		"#" + special_code;
	map<string, size_t>::iterator found = function_index_.find(key);
	if (found != function_index_.end())
		return functions_[found->second];
	LowFunctionInfo info;
	info.scope = scope;
	info.name = name;
	info.type = type;
	info.is_method = true;
	info.special_code = special_code;
	string base = LowerScopePath(scope) + LowerSanitizeName(name);
	size_t overload = special_code.empty()
		? LowerMemberOverloadIndex(scope, name, type) : 0;
	if (overload)
		base += "__ov" + to_string(overload + 1);
	if (special_code == "C2" || special_code == "D2")
		base += "__base_entry";
	else if (special_code == "D0")
		// PA17: the deleting destructor entry (vtable slot pair).
		base += "__deleting_entry";
	info.low_name = UniqueSymbol(base);
	info.internal = LowerInUnnamedNamespace(scope);
	// PA21: a constructor-template specialization entry mangles with
	// its template-argument list (C1I<args>E).
	const FunctionSpecialization* ctor_spec = 0;
	if (special_code == "C1" || special_code == "C2")
		if (const ClassInfo* cls = MethodClass(type))
			for (size_t i = 0; !ctor_spec && i < cls->ctors.size(); i++)
				if (cls->ctors[i].tmpl_spec &&
				    CtorEntryMatches(cls->ctors[i].type, type))
					ctor_spec = cls->ctors[i].tmpl_spec;
	if (ctor_spec)
	{
		info.fn_spec = ctor_spec;
		info.object_name = MangleMemberFunctionTemplateObjectName(
			scope, *ctor_spec, special_code);
		if (special_code == "C1")
			info.alias_object = MangleMemberFunctionTemplateObjectName(
				scope, *ctor_spec, "C2");
	}
	else
	{
		info.object_name = MangleMemberFunctionObjectName(
			scope, name, type, special_code);
		// Complete and base entries are identical without virtual
		// bases; an emitted complete entry carries the base-entry
		// alias.
		if (special_code == "C1")
			info.alias_object =
				MangleMemberFunctionObjectName(scope, name, type, "C2");
		else if (special_code == "D1")
			info.alias_object =
				MangleMemberFunctionObjectName(scope, name, type, "D2");
	}
	ESpecialFunction special = SF_NONE;
	if (special_code == "C1" || special_code == "C2")
		special = SF_CONSTRUCTOR;
	else if (special_code == "D1" || special_code == "D2" ||
	         special_code == "D0")
		special = SF_DESTRUCTOR;
	map<string, const SemNode*>::iterator def = member_defs_.find(
		MemberDefinitionKey(scope, name, type, special));
	if (def != member_defs_.end())
	{
		info.defined = true;
		info.weak = def->second->inline_def;
		info.definition = def->second;
		info.unwind_no = def->second->unwind_no;
	}
	info.index = functions_.size();
	function_index_[key] = functions_.size();
	functions_.push_back(info);
	return functions_.back();
}

void LowerProgram::RegisterFunction(const SemNode& item, bool defined)
{
	if (item.is_method)
	{
		// An out-of-class member function definition: source-owned,
		// emitted unconditionally.
		LowFunctionInfo& member = MemberFunctionEntry(
			item.entity_scope, item.entity_name, item.type, "");
		if (defined)
		{
			member.defined = true;
			member.weak = false;
			member.definition = &item;
			member.unwind_no = item.unwind_no;
		}
		return;
	}
	LowFunctionInfo& info = FunctionEntry(item.entity_scope,
	                                      item.entity_name, item.type,
	                                      item.fn_spec);
	if (item.c_linkage)
	{
		info.c_linkage = true;
		info.object_name.clear();
	}
	if (defined)
	{
		if (info.defined)
			throw runtime_error("redefinition of " + item.entity_name);
		info.defined = true;
		info.definition = &item;
		info.unwind_no = item.unwind_no;
		// 7.1.2p4: inline definitions emit weak, on demand. Inline
		// (non-constexpr) bodies keep the conservative demand sweep;
		// constexpr bodies used only in constant expressions must not
		// propagate demand.
		if (item.inline_def)
		{
			info.weak = true;
			info.friend_def = item.entity_scope &&
				item.entity_scope->kind == SCOPE_NAMESPACE &&
				!item.is_constexpr_fn;
		}
	}
}

string LowerProgram::UniqueSymbol(const string& base)
{
	if (!symbols_.count(base))
	{
		symbols_.insert(base);
		return base;
	}
	for (int i = 2;; i++)
	{
		string candidate = base + "__ov" + to_string(i);
		if (!symbols_.count(candidate))
		{
			symbols_.insert(candidate);
			return candidate;
		}
	}
}

string LowerProgram::GlobalRef(const Scope* scope, const string& name)
{
	// A hoisted function-local static shadows the collapsed key.
	map<string, size_t>::iterator local =
		global_index_.find(LocalStaticKey(scope, name));
	if (local != global_index_.end())
	{
		globals_[local->second].used = true;
		return "@" + globals_[local->second].low_name;
	}
	LowGlobalInfo& info = GlobalEntry(scope, name);
	info.used = true;
	return "@" + info.low_name;
}

string LowerProgram::FunctionRef(const Scope* scope, const string& name,
                                 const TypePtr& type,
                                 const FunctionSpecialization* spec)
{
	LowFunctionInfo& info = FunctionEntry(scope, name, type, spec);
	DemandFunction(info);
	return "@" + info.low_name;
}

const SemNode* LowerProgram::MemberDefinitionFor(const SemNode& callee)
{
	map<string, const SemNode*>::iterator def = member_defs_.find(
		MemberDefinitionKey(callee.entity_scope, callee.entity_name,
		                    callee.type, SF_NONE));
	return def == member_defs_.end() ? 0 : def->second;
}

string LowerProgram::MemberFunctionRef(const SemNode& callee)
{
	const char* code = "";
	switch (callee.special)
	{
	case SF_CONSTRUCTOR: code = "C1"; break;
	case SF_CONSTRUCTOR_BASE: code = "C2"; break;
	case SF_DESTRUCTOR: code = "D1"; break;
	case SF_DESTRUCTOR_BASE: code = "D2"; break;
	default:
		break;
	}
	LowFunctionInfo& info = MemberFunctionEntry(
		callee.entity_scope, callee.entity_name, callee.type, code);
	DemandFunction(info);
	return "@" + info.low_name;
}

// PA27: whether the (already lowered) callee body carries a
// null-guarded displaced-base adjustment; callers under cleanups wrap
// such calls in a lazy dispatch region even when the sema unwind fact
// stayed non-throwing.
bool LowerProgram::CalleeGuardedBody(const SemNode& callee)
{
	if (callee.kind != SN_CALLEE || !callee.entity_scope)
		return false;
	return CalleeEntryInfo(callee).guarded_body;
}

// PA27: the registry entry a direct call resolves to (for the hidden
// vbase-pointer signature), without demanding it.
LowFunctionInfo& LowerProgram::CalleeEntryInfo(const SemNode& callee)
{
	if (callee.is_method || callee.special != SF_NONE)
	{
		const char* code = "";
		switch (callee.special)
		{
		case SF_CONSTRUCTOR: code = "C1"; break;
		case SF_CONSTRUCTOR_BASE: code = "C2"; break;
		case SF_DESTRUCTOR: code = "D1"; break;
		case SF_DESTRUCTOR_BASE: code = "D2"; break;
		default:
			break;
		}
		return MemberFunctionEntry(callee.entity_scope,
		                           callee.entity_name, callee.type, code);
	}
	return FunctionEntry(callee.entity_scope, callee.entity_name,
	                     callee.type, callee.fn_spec);
}

void LowerProgram::RequireEhRuntime()
{
	needs_eh_runtime_ = true;
}

void LowerProgram::DemandElidedCtor(const SemNode& callee)
{
	if (callee.kind != SN_CALLEE || !callee.entity_scope)
		return;
	const char* code = "";
	switch (callee.special)
	{
	case SF_CONSTRUCTOR: code = "C1"; break;
	case SF_CONSTRUCTOR_BASE: code = "C2"; break;
	case SF_DESTRUCTOR: code = "D1"; break;
	case SF_DESTRUCTOR_BASE: code = "D2"; break;
	default:
		break;
	}
	LowFunctionInfo& info =
		callee.is_method || callee.special != SF_NONE
			? MemberFunctionEntry(callee.entity_scope,
			                      callee.entity_name, callee.type,
			                      code)
			: FunctionEntry(callee.entity_scope, callee.entity_name,
			                callee.type, callee.fn_spec);
	// The elided call itself emits nothing; a synthesized body it
	// selected is implicitly defined, so the user-provided members
	// that body odr-uses must still appear. Demand state is monotonic,
	// so each synthesized tree walks at most once.
	if (info.definition && info.definition->synthesized &&
	    demanded_trees_.insert(info.definition).second)
		DemandTreeCallees(*info.definition);
}

// PA20: the analyzed in-class initialization actions behind an
// initializer-less storage definition (9.4.2p3), or null.
const SemNode* LowerProgram::ImageInitActions(const SemNode* node) const
{
	for (size_t u = 0; u < units_.size(); u++)
	{
		map<const SemNode*, SemNodePtr>::const_iterator actions =
			units_[u]->const_image_inits.find(node);
		if (actions != units_[u]->const_image_inits.end())
			return actions->second.get();
	}
	return 0;
}

// PA20: an image-backed definition whose image renders emits the
// constant value statically and drops its initialization actions; the
// constructors those actions selected stay odr-used (3.2p3). False
// when the definition must initialize dynamically.
bool LowerProgram::TryImageBackedInit(LowGlobalInfo& info)
{
	if (!ImageBacked(info) || !EnsureImageText(info))
		return false;
	DemandImageInitCallees(*info.node);
	if (const SemNode* held = ImageInitActions(info.node))
		DemandImageInitCallees(*held);
	return true;
}

// PA20: an image-backed definition drops its initialization actions,
// but the constructors those actions selected are still odr-used
// (3.2p3): user-provided bodies emit; synthesized bodies only
// propagate the demand into the members they use.
void LowerProgram::DemandImageInitCallees(const SemNode& item)
{
	for (size_t i = 0; i < item.children.size(); i++)
	{
		const SemNode& action = *item.children[i];
		// A call-initialized image (a constexpr function result): the
		// callee tree stays odr-used even though the call dropped.
		if (action.kind == SN_CALL_EXPRESSION)
		{
			if (demanded_trees_.insert(&action).second)
				DemandTreeCallees(action);
			continue;
		}
		if (action.kind != SN_CONSTRUCTOR_ACTION ||
		    action.children.empty() ||
		    action.children[0]->kind != SN_CALL_EXPRESSION ||
		    action.children[0]->children.empty())
			continue;
		const SemNode& callee = *action.children[0]->children[0];
		if (callee.kind != SN_CALLEE || !callee.entity_scope)
			continue;
		const char* code = callee.special == SF_CONSTRUCTOR_BASE
			? "C2" : "C1";
		LowFunctionInfo& info = MemberFunctionEntry(
			callee.entity_scope, callee.entity_name, callee.type,
			code);
		if (info.defined && info.definition &&
		    !info.definition->synthesized)
			DemandFunction(info);
		else
			DemandElidedCtor(callee);
	}
}

void LowerProgram::DemandTrivialCtorBody(const SemNode& callee)
{
	if (callee.kind != SN_CALLEE || !callee.entity_scope ||
	    callee.special != SF_CONSTRUCTOR)
		return;
	LowFunctionInfo& info = MemberFunctionEntry(
		callee.entity_scope, callee.entity_name, callee.type, "C1");
	if (info.defined && info.definition &&
	    info.definition->synthesized)
		DemandFunction(info);
}

void LowerProgram::DemandTreeCallees(const SemNode& node)
{
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		if (child.kind == SN_CALLEE && child.entity_scope)
		{
			const char* code = "";
			switch (child.special)
			{
			case SF_CONSTRUCTOR: code = "C1"; break;
			case SF_CONSTRUCTOR_BASE: code = "C2"; break;
			case SF_DESTRUCTOR: code = "D1"; break;
			case SF_DESTRUCTOR_BASE: code = "D2"; break;
			default:
				break;
			}
			LowFunctionInfo& info =
				child.is_method || child.special != SF_NONE
					? MemberFunctionEntry(child.entity_scope,
					                      child.entity_name,
					                      child.type, code)
					: FunctionEntry(child.entity_scope,
					                child.entity_name, child.type,
					                child.fn_spec);
			if (info.definition)
			{
				// A member the elided body odr-uses appears whether
				// user-provided or synthesized; only the elided body
				// itself stays unprinted.
				DemandFunction(info);
				if (info.definition->synthesized &&
				    demanded_trees_.insert(info.definition).second)
					DemandTreeCallees(*info.definition);
			}
		}
		DemandTreeCallees(child);
	}
}

// PA25: a specialization whose arguments carry closure classes
// creates their operator() entries with it (the reference's emission
// order); only real calls demand the bodies.
void LowerProgram::TouchClosureOperators(const FunctionSpecialization& spec)
{
	for (size_t i = 0; i < spec.args.size(); i++)
	{
		const TemplateArg& arg = spec.args[i];
		if (!arg.type)
			continue;
		TypePtr bare = RemoveTopCv(
			IsReferenceType(arg.type) ? arg.type->target : arg.type);
		if (bare->kind != TK_CLASS || !bare->named->is_closure ||
		    !bare->named->class_record)
			continue;
		const ClassInfo* cls = bare->named->class_record;
		const ScopeBinding* op =
			FindOwnBinding(*cls->members, "operator ()");
		if (!op || !op->type || op->type->kind != TK_FUNCTION)
			continue;
		TypePtr class_type = MakeNamedType(TK_CLASS, bare->named);
		class_type = MakeCvQualifiedType(
			class_type, op->type->is_const, op->type->is_volatile);
		vector<TypePtr> params;
		params.push_back(MakePointerType(class_type, false, false));
		for (size_t p = 0; p < op->type->parameters.size(); p++)
			params.push_back(op->type->parameters[p]);
		MemberFunctionEntry(cls->members, "operator ()",
		                    MakeFunctionType(op->type->target, params,
		                                     op->type->variadic),
		                    "");
	}
}

bool LowerProgram::CalleeMayUnwind(const SemNode& callee)
{
	if (callee.unwind_no)
		return false;
	if (callee.kind != SN_CALLEE || !callee.entity_scope)
		return true;
	if (callee.is_method || callee.special != SF_NONE)
	{
		const char* code = "";
		switch (callee.special)
		{
		case SF_CONSTRUCTOR: code = "C1"; break;
		case SF_CONSTRUCTOR_BASE: code = "C2"; break;
		case SF_DESTRUCTOR: code = "D1"; break;
		case SF_DESTRUCTOR_BASE: code = "D2"; break;
		default:
			break;
		}
		return !MemberFunctionEntry(callee.entity_scope,
		                            callee.entity_name, callee.type,
		                            code).unwind_no;
	}
	return !FunctionEntry(callee.entity_scope, callee.entity_name,
	                      callee.type).unwind_no;
}

string LowerProgram::StringLiteralRef(const SemNode& node)
{
	TypePtr element = RemoveTopCv(node.type->target);
	string key = DescribeType(element) + "|" + node.string_bytes;
	map<string, size_t>::iterator found = string_index_.find(key);
	if (found != string_index_.end())
		return "@" + strings_[found->second].low_name;
	LowStringLiteral literal;
	literal.low_name =
		UniqueSymbol("__strlit__" + to_string(strings_.size() + 1));
	literal.element = element;
	literal.bytes = node.string_bytes;
	string_index_[key] = strings_.size();
	strings_.push_back(literal);
	return "@" + strings_.back().low_name;
}

// Demand-marks a function entry. A flip behind the sweep below re-arms
// the rescan floor so the sweep revisits only that suffix.
void LowerProgram::DemandFunction(LowFunctionInfo& info)
{
	if (info.used)
		return;
	info.used = true;
	if (info.defined && info.body_text.empty() &&
	    info.index < lower_floor_)
		lower_floor_ = info.index;
	// PA17: an emitted base entry of a user-provided constructor or
	// destructor of a class with a virtual destructor also prints the
	// complete entry (which carries the base-entry alias), mirroring
	// the reference comdat-style member emission. PA18: an
	// instantiated specialization's members pair the same way, but
	// only while the two entries are one alias unit - with no bases
	// the complete and base entries do identical work and the
	// reference prints one definition carrying both symbols; a base
	// makes them distinct definitions, each emitted on its own demand
	// (witness: inherited-class-template-conversion-operator). PA23: a
	// demand fired from inside another specialization's own (non
	// -synthesized) constructor body also stays a lone base entry;
	// only source-owned and inheriting-constructor contexts drag the
	// complete entry along.
	if ((info.special_code == "C2" || info.special_code == "D2") &&
	    info.defined && info.definition && !info.definition->synthesized)
	{
		const ClassInfo* cls = MethodClass(info.type);
		bool from_spec_ctor = lowering_context_ &&
			(lowering_context_->special_code == "C1" ||
			 lowering_context_->special_code == "C2") &&
			lowering_context_->definition &&
			!lowering_context_->definition->synthesized &&
			!lowering_context_->fn_spec &&
			lowering_context_->scope && lowering_context_->scope->entity &&
			lowering_context_->scope->entity->spec_template;
		// PA27: a constructor/destructor context whose class carries
		// virtual bases pairs every user-provided subobject entry; a
		// polymorphic context pairs the entries on its primary chain
		// (reference-observed comdat behavior).
		bool ctx_pair = false;
		if (lowering_context_ && lowering_context_->is_method &&
		    lowering_context_->weak &&
		    !lowering_context_->special_code.empty())
		{
			const ClassInfo* ctx_cls =
				MethodClass(lowering_context_->type);
			if (ctx_cls && ClassHasVBases(*ctx_cls))
				ctx_pair = true;
			else if (ctx_cls && ctx_cls->is_polymorphic)
				for (const ClassInfo* at = ctx_cls->base; at;
				     at = at->base)
					if (at == cls)
					{
						ctx_pair = true;
						break;
					}
		}
		bool comdat_pair = cls &&
			((cls->is_polymorphic && cls->dtor_virtual) || ctx_pair ||
			 (cls->entity && cls->entity->spec_template &&
			  !cls->base && !from_spec_ctor));
		// A constructor-template specialization's entries emit each
		// on their own demand (witness: the inherited constructor
		// -template forwarding references carry a lone C2).
		if (comdat_pair && info.special_code == "C2")
			for (size_t i = 0; i < cls->ctors.size(); i++)
			{
				const ClassCtor& ctor = cls->ctors[i];
				if (!ctor.tmpl_spec ||
				    ctor.type->parameters.size() + 1 !=
				        info.type->parameters.size())
					continue;
				bool same = true;
				for (size_t p = 0; p < ctor.type->parameters.size();
				     p++)
					if (!TypeEquals(ctor.type->parameters[p],
					                info.type->parameters[p + 1]))
						same = false;
				if (same)
				{
					comdat_pair = false;
					break;
				}
			}
		if (comdat_pair)
			DemandFunction(MemberFunctionEntry(
				info.scope, info.name, info.type,
				info.special_code == "C2" ? "C1" : "D1"));
	}
}


// Lowers the reachable definitions: source-owned roots first, then
// hidden-friend bodies (whose references count as semantic demand even
// when the friend itself never prints), then whatever those marked.
// Each sweep runs in entry order from the rescan floor; entries below
// the floor are already lowered or had no demand flip since last
// checked, so repeated whole-table walks are avoided.
void LowerProgram::LowerUsedFunctions()
{
	for (int phase = 0; phase < 2; phase++)
	{
		lower_floor_ = 0;
		while (lower_floor_ < functions_.size())
		{
			size_t start = lower_floor_;
			lower_floor_ = functions_.size();
			for (size_t i = start; i < functions_.size(); i++)
			{
				LowFunctionInfo& info = functions_[i];
				if (!info.defined || !info.body_text.empty() ||
				    info.extern_suppressed)
					continue;
				// A poisoned instantiated friend body only lowers on
				// real demand (its stored error reports then).
				bool friend_body = phase > 0 && info.friend_def &&
					(!info.definition ||
					 info.definition->instantiation_error.empty());
				if (info.weak && !info.used && !friend_body)
					continue;
				const LowFunctionInfo* saved = lowering_context_;
				lowering_context_ = &info;
				FunctionLowerer lowerer(*this, *info.definition, info);
				info.body_text = lowerer.Lower();
				lowering_context_ = saved;
			}
		}
	}
}

void LowerProgram::LowerHelper(LowFunctionInfo& info,
                               const SemNode& definition)
{
	info.definition = &definition;
	info.defined = true;
	const LowFunctionInfo* saved = lowering_context_;
	lowering_context_ = &info;
	FunctionLowerer lowerer(*this, definition, info);
	info.body_text = lowerer.Lower();
	lowering_context_ = saved;
}

// One dynamic global initializer: when the value (or referent
// address) is not a renderable constant, @__cppgm_init stores it.
void LowerProgram::AppendDynamicInit(LowGlobalInfo& info,
                                     const SemNode& child, bool ref,
                                     SemNode& init_def)
{
	LowerConst value;
	if (EvaluateLowerConst(child, value))
		return;
	info.dynamic_init = true;
	SemNodePtr target = MakeSemNode(SN_ID_EXPRESSION);
	target->name = info.node->entity_name;
	target->type = info.type;
	target->category = VC_LVALUE;
	target->entity_scope = info.node->entity_scope;
	target->entity_name = info.node->entity_name;
	SemNodePtr assign = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
	assign->type = ref ? info.type : RemoveTopCv(info.type);
	assign->category = VC_LVALUE;
	assign->has_op = true;
	assign->op = OP_ASS;
	assign->op_spelling = "=";
	assign->member_ref = ref;
	assign->children.push_back(std::move(target));
	assign->children.push_back(CloneSemNode(child));
	SemNodePtr statement = MakeSemNode(SN_EXPRESSION_STATEMENT);
	statement->children.push_back(std::move(assign));
	init_def.children.push_back(std::move(statement));
}

// PA18: a dynamically initialized thread-local object constructs on
// first use: an internal thread-local i64 guard and an internal
// `<name>__tls_init` function running the construction actions once
// per thread (the backend's TLS wrapper calls it).
void LowerProgram::BuildTlsGuardedInit(size_t global_index,
                                       vector<SemNodePtr>& actions)
{
	string object_name = globals_[global_index].low_name;
	LowGlobalInfo guard;
	guard.type = MakeFundamentalType(FT_LONG_INT);
	guard.low_name = UniqueSymbol(object_name + "__tls_guard");
	guard.internal = true;
	guard.is_thread_local = true;
	guard.defined = true;
	global_index_["#tls_guard#" + object_name] = globals_.size();
	globals_.push_back(guard);

	SemNodePtr guarded = MakeSemNode(SN_STATIC_GUARD);
	guarded->name = globals_.back().low_name;
	for (size_t i = 0; i < actions.size(); i++)
		guarded->children.push_back(std::move(actions[i]));
	SemNodePtr init_def = MakeSemNode(SN_FUNCTION_DEFINITION);
	init_def->type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                  vector<TypePtr>(), false);
	init_def->children.push_back(std::move(guarded));

	LowFunctionInfo info;
	info.scope = 0;
	info.name = object_name + "__tls_init";
	info.type = init_def->type;
	info.low_name = UniqueSymbol(object_name + "__tls_init");
	info.internal = true;
	info.index = functions_.size();
	functions_.push_back(info);
	LowerHelper(functions_.back(), *init_def);
	helper_defs_.push_back(std::move(init_def));
}

// Builds @__cppgm_init / @__cppgm_fini from the registered
// namespace-scope objects: construction in declaration order,
// destruction in reverse declaration order (3.6.2/3.6.3 subset).
void LowerProgram::BuildLifetimeHelpers()
{
	SemNodePtr init_def = MakeSemNode(SN_FUNCTION_DEFINITION);
	init_def->type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                  vector<TypePtr>(), false);
	SemNodePtr fini_def = MakeSemNode(SN_FUNCTION_DEFINITION);
	fini_def->type = init_def->type;
	bool any_class_object = false;
	for (size_t i = 0; i < globals_.size(); i++)
	{
		LowGlobalInfo& info = globals_[i];
		if (!info.defined || !info.node)
			continue;
		TypePtr inner = info.type;
		while (inner->kind == TK_ARRAY)
			inner = inner->target;
		bool is_class = RemoveTopCv(inner)->kind == TK_CLASS;
		// PA20: an image-backed definition whose image renders carries
		// its constant value statically; no initialization actions run
		// for it, but the constructors they selected stay odr-used. An
		// unrenderable image (engine-internal addresses, bit-fields)
		// falls through and initializes dynamically.
		if (TryImageBackedInit(info))
		{
			AppendImageInitTouch(info, is_class, *init_def);
			continue;
		}
		// PA18: a weak (instantiated static member) object does not by
		// itself anchor the init helper; a thread-local object
		// constructs behind its own first-use guard, not in the
		// program-wide init helper.
		bool tls_dynamic = info.is_thread_local && is_class;
		// A class object initialized through a constructor - even a
		// trivial or elided one - anchors the init helper; an
		// aggregate braced initialization does not (the reference
		// presentation pins both shapes).
		bool ctor_driven = info.node->children.empty() &&
			!info.node->has_explicit_init;
		for (size_t j = 0; j < info.node->children.size(); j++)
			if (info.node->children[j]->kind == SN_CONSTRUCTOR_ACTION)
				ctor_driven = true;
		if (is_class && !info.weak && !tls_dynamic && ctor_driven)
			any_class_object = true;
		// PA20: an initializer-less storage definition (9.4.2p3) whose
		// image cannot render initializes dynamically from the analyzed
		// in-class actions instead.
		const SemNode* action_item = info.node;
		if (action_item->children.empty())
			if (const SemNode* held = ImageInitActions(info.node))
				action_item = held;
		const SemNode& item = *action_item;
		vector<SemNodePtr> tls_actions;
		for (size_t j = 0; j < item.children.size(); j++)
		{
			const SemNode& child = *item.children[j];
			if (child.kind == SN_CONSTRUCTOR_ACTION)
			{
				if (child.trivial_init)
					continue;
				if (tls_dynamic)
					tls_actions.push_back(CloneSemNode(child));
				else
					init_def->children.push_back(CloneSemNode(child));
			}
			else if (child.kind == SN_DESTRUCTOR_ACTION)
				fini_def->children.push_back(CloneSemNode(child));
			else if (child.kind == SN_EXPRESSION_STATEMENT)
			{
				// Aggregate member stores run dynamically.
				if (tls_dynamic)
					tls_actions.push_back(CloneSemNode(child));
				else
					init_def->children.push_back(CloneSemNode(child));
			}
			else if (IsReferenceType(info.type) && j == 0)
				// The reference binds dynamically: the helper stores
				// the referent's address into the pointer object.
				AppendDynamicInit(info, child, true, *init_def);
			else if (!is_class && info.type->kind != TK_ARRAY &&
			         !IsReferenceType(info.type) && j == 0 &&
			         child.kind != SN_BRACED_INIT_LIST)
				// A scalar initializer: constant expressions render
				// statically, everything else stores in @__cppgm_init.
				AppendDynamicInit(info, child, false, *init_def);
		}
		if (!tls_actions.empty())
			// May grow globals_; `info` is not used past this point.
			BuildTlsGuardedInit(i, tls_actions);
	}
	// Finalization actions run in reverse declaration order.
	for (size_t i = 0, j = fini_def->children.size(); i < j / 2; i++)
		fini_def->children[i].swap(fini_def->children[j - 1 - i]);
	if (any_class_object || !init_def->children.empty())
		RegisterLifetimeHelper("__cppgm_init", "init",
		                       std::move(init_def));
	if (!fini_def->children.empty())
		RegisterLifetimeHelper("__cppgm_fini", "fini",
		                       std::move(fini_def));
}

// PA23: a call-copy-initialized weak image object keeps its
// init-order anchor - the dropped call and empty copy leave the
// destination address evaluation (the checked variable-template
// shape).
void LowerProgram::AppendImageInitTouch(const LowGlobalInfo& info,
                                        bool is_class, SemNode& init_def)
{
	if (!info.weak || !is_class || !info.node->has_explicit_init ||
	    info.node->children.size() != 1 ||
	    info.node->children[0]->kind != SN_CALL_EXPRESSION)
		return;
	SemNodePtr touch = MakeSemNode(SN_EXPRESSION_STATEMENT);
	SemNodePtr id = MakeSemNode(SN_ID_EXPRESSION);
	id->name = info.node->entity_name;
	id->type = info.type;
	id->category = VC_LVALUE;
	id->entity_scope = info.node->entity_scope;
	id->entity_name = info.node->entity_name;
	touch->children.push_back(std::move(id));
	init_def.children.push_back(std::move(touch));
}

void LowerProgram::RegisterLifetimeHelper(const char* name,
                                          const char* role,
                                          SemNodePtr def)
{
	LowFunctionInfo info;
	info.scope = 0;
	info.name = name;
	info.type = def->type;
	info.low_name = UniqueSymbol(name);
	info.internal = true;
	info.role = role;
	info.index = functions_.size();
	functions_.push_back(info);
	LowerHelper(functions_.back(), *def);
	helper_defs_.push_back(std::move(def));
}

// PA17 polymorphic emission: external declares, the pure-virtual
// stand-in, RTTI records, and the vtable definitions merge into the
// declare-global, declare-function, and global output phases.
void LowerProgram::AppendPolymorphicSections(vector<string>* sections)
{
	for (size_t i = 0; i < poly_declare_globals_.size(); i++)
		sections[0].push_back(poly_declare_globals_[i]);
	if (!pure_virtual_declare_.empty())
		sections[1].push_back(pure_virtual_declare_);
	for (size_t i = 0; i < poly_globals_.size(); i++)
		sections[2].push_back(poly_globals_[i]);
	for (size_t i = 0; i < vtables_.size(); i++)
		if (!vtables_[i].text.empty())
			sections[2].push_back(vtables_[i].text);
}

void LowerProgram::Write(ostream& out)
{
	// Lifetime helpers register dynamic-init facts before the globals
	// render; globals render before bodies lower so string-literal
	// demand order matches the canonical presentation.
	BuildLifetimeHelpers();
	for (size_t i = 0; i < globals_.size(); i++)
		if (globals_[i].defined)
			globals_[i].init_text = RenderGlobal(globals_[i]);
	LowerUsedFunctions();
	// PA17: rendering a demanded vtable demands its slot definitions
	// (and their bodies may demand further vtables), so the two sweeps
	// alternate to a fixpoint.
	while (RenderPendingVTables())
		LowerUsedFunctions();
	// PA20: function-local statics register while their enclosing
	// bodies lower; render whatever the first pass missed.
	for (size_t i = 0; i < globals_.size(); i++)
		if (globals_[i].defined && globals_[i].init_text.empty())
			globals_[i].init_text = RenderGlobal(globals_[i]);

	vector<string> sections[5];
	for (size_t i = 0; i < globals_.size(); i++)
	{
		const LowGlobalInfo& info = globals_[i];
		if (info.defined || !info.used)
			continue;
		string line = "declare global @" + info.low_name;
		TypePtr declared_inner = info.type;
		while (declared_inner->kind == TK_ARRAY)
			declared_inner = declared_inner->target;
		if (info.type->kind != TK_ARRAY &&
		    RemoveTopCv(declared_inner)->kind != TK_CLASS)
			line += " : " + LowerValueType(info.type);
		line += GlobalMetadata(info);
		sections[0].push_back(line);
	}
	for (size_t i = 0; i < functions_.size(); i++)
	{
		const LowFunctionInfo& info = functions_[i];
		if ((info.defined && !info.extern_suppressed) || !info.used ||
		    info.deleted)
			continue;
		sections[1].push_back(RenderFunctionDeclare(info));
	}
	AppendTlsWrapperDeclares(sections[1]);
	for (size_t i = 0; i < strings_.size(); i++)
	{
		string body;
		const LowStringLiteral& literal = strings_[i];
		unsigned long long width = TypeSize(literal.element);
		// String data items render their raw unsigned code units.
		for (size_t at = 0; at + width <= literal.bytes.size();
		     at += width)
			body += "  " + LowerValueType(literal.element) + " " +
				to_string(LittleEndianValue(
				    literal.bytes.substr(at, width))) + "\n";
		sections[2].push_back("global @" + literal.low_name +
		                      " [binding=internal] = {\n" + body + "}");
	}
	for (size_t i = 0; i < globals_.size(); i++)
		if (globals_[i].defined)
			sections[2].push_back(globals_[i].init_text);
	AppendPolymorphicSections(sections);
	for (size_t i = 0; i < functions_.size(); i++)
	{
		const LowFunctionInfo& info = functions_[i];
		if (!info.defined || info.body_text.empty())
			continue;
		if (info.weak && !info.used)
			continue;
		sections[3].push_back(info.body_text);
	}
	// PA27: vtable entry thunks print after the ordinary bodies.
	for (size_t i = 0; i < thunk_texts_.size(); i++)
		sections[3].push_back(thunk_texts_[i]);
	if (needs_eh_runtime_)
	{
		vector<string> declares;
		declares.push_back(
			"declare function @__external_runtime___Unwind_Resume() -> "
			"void [return=noreturn, role=eh_resume, linkage=c, "
			"binding=strong, object=_Unwind_Resume]");
		declares.push_back(
			"declare function @__external_runtime____gxx_personality_v0()"
			" -> void [role=eh_personality, linkage=c, binding=strong, "
			"object=__gxx_personality_v0]");
		sections[1].insert(sections[1].begin(), declares.begin(),
		                   declares.end());
	}
	// PA25 external runtime helpers (__cxa_bad_typeid, __dynamic_cast,
	// ...) declared on first use.
	sections[1].insert(sections[1].begin(), runtime_declares_.begin(),
	                   runtime_declares_.end());

	// Aliases follow the definitions without a separating blank line.
	for (size_t i = 0; i < functions_.size(); i++)
	{
		const LowFunctionInfo& info = functions_[i];
		if (!info.defined || info.body_text.empty() ||
		    (info.weak && !info.used) || info.alias_object.empty())
			continue;
		sections[4].push_back("alias object " + info.alias_object +
		                      " = @" + info.low_name);
	}
	bool first = true;
	for (int kind = 0; kind < 5; kind++)
	{
		bool separate = kind != 4;
		for (size_t i = 0; i < sections[kind].size(); i++)
		{
			if (!first && i == 0 && separate)
				out << "\n";
			out << sections[kind][i] << "\n";
			first = false;
		}
	}
}

