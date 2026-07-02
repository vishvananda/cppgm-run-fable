#include "lowering/lower_program.h"

#include <stdexcept>

#include "lowering/lower_const.h"
#include "lowering/lower_function.h"
#include "lowering/lower_name.h"
#include "lowering/lower_types.h"
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

// The fundamental value space of an integral or enumeration type.
EFundamentalType ValueFund(const TypePtr& type)
{
	if (type->kind == TK_ENUM)
		return type->named->enum_underlying;
	return type->fundamental;
}

// Floating global initializers keep the literal spelling; the type
// suffix follows the destination type.
string FloatInitToken(const TypePtr& type, const LowerConst& value)
{
	string token = value.float_token;
	char last = token.empty() ? 0 : token[token.size() - 1];
	bool suffixed = last == 'f' || last == 'F' || last == 'l' ||
		last == 'L';
	if (!suffixed && type->fundamental == FT_FLOAT)
		token += "f";
	else if (!suffixed && type->fundamental == FT_LONG_DOUBLE)
		token += "L";
	return token;
}

// --- branch-fold analysis (BranchSpellsFnPointerAddress) --------------------

bool TreeNamesEntity(const SemNode& node, const Scope* scope,
                     const string& name)
{
	if (node.kind == SN_ID_EXPRESSION && node.entity_scope == scope &&
	    node.entity_name == name)
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (TreeNamesEntity(*node.children[i], scope, name))
			return true;
	return false;
}

bool TreeHasCall(const SemNode& node)
{
	if (node.kind == SN_CONSTRUCTOR_ACTION)
		// Trivial and elided actions lower to no runtime call (their
		// skeleton call node never executes; LowerConstructorAction).
		return !node.trivial_init && !node.elided;
	if (node.kind == SN_CALL_EXPRESSION ||
	    node.kind == SN_DESTRUCTOR_ACTION)
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (TreeHasCall(*node.children[i]))
			return true;
	return false;
}

// Whether this global contributes an init-time action that can call a
// function (mirrors BuildLifetimeHelpers's action selection: trivial
// construction and static-rendering initializers run nothing).
bool GlobalInitRunsCall(const LowGlobalInfo& info)
{
	if (!info.defined || !info.node)
		return false;
	for (size_t j = 0; j < info.node->children.size(); j++)
	{
		const SemNode& child = *info.node->children[j];
		if (child.kind == SN_CONSTRUCTOR_ACTION ||
		    child.kind == SN_EXPRESSION_STATEMENT ||
		    info.dynamic_init)
			if (TreeHasCall(child))
				return true;
	}
	return false;
}

// A provably non-null initializer: the address of a named entity, or
// a function lvalue decaying to its address.
bool InitIsNonNullAddress(const SemNode& node)
{
	if (node.kind == SN_UNARY_EXPRESSION && node.op == OP_AMP &&
	    !node.children.empty() &&
	    node.children[0]->kind == SN_ID_EXPRESSION)
		return true;
	return node.kind == SN_ID_EXPRESSION && node.type &&
		RemoveTopCv(node.type)->kind == TK_FUNCTION;
}

// Whether the tree writes to, takes the address of, or reference-binds
// the object - anything that could change the stored value or alias
// the storage. Unrecognized aliasing forms classify as unsafe.
bool TreeHasUnsafeUse(const SemNode& node, const Scope* scope,
                      const string& name)
{
	switch (node.kind)
	{
	case SN_ASSIGNMENT_EXPRESSION:
		if (!node.children.empty() &&
		    TreeNamesEntity(*node.children[0], scope, name))
			return true;
		// A reference-binding assignment aliases its source.
		if (node.member_ref && node.children.size() > 1 &&
		    TreeNamesEntity(*node.children[1], scope, name))
			return true;
		break;
	case SN_STORAGE_COPY:
		if (!node.children.empty() &&
		    TreeNamesEntity(*node.children[0], scope, name))
			return true;
		break;
	case SN_UNARY_EXPRESSION:
		if (node.op == OP_AMP && !node.children.empty() &&
		    TreeNamesEntity(*node.children[0], scope, name))
			return true;
		break;
	case SN_POSTFIX_EXPRESSION:
		if (!node.children.empty() &&
		    TreeNamesEntity(*node.children[0], scope, name))
			return true;
		break;
	case SN_CALL_EXPRESSION:
	{
		if (node.children.empty())
			break;
		const SemNode& callee = *node.children[0];
		size_t args = node.children.size() - 1;
		if (!callee.type || callee.type->kind != TK_FUNCTION)
		{
			// Indirect or unusual callee: stay conservative.
			for (size_t i = 1; i < node.children.size(); i++)
				if (TreeNamesEntity(*node.children[i], scope, name))
					return true;
			break;
		}
		// Method calls carry the object address first; align the
		// parameter list to the trailing arguments.
		size_t params = callee.type->parameters.size();
		size_t shift = args > params ? args - params : 0;
		for (size_t i = 1; i < node.children.size(); i++)
		{
			size_t slot = i - 1;
			bool by_ref = slot < shift;
			if (!by_ref && slot - shift < params)
				by_ref = IsReferenceType(
					callee.type->parameters[slot - shift]);
			if (by_ref && TreeNamesEntity(*node.children[i], scope, name))
				return true;
		}
		break;
	}
	case SN_CONSTRUCTOR_ACTION:
	case SN_NEW_INIT:
	case SN_NEW_ARRAY:
		// Constructor parameter types are not visible here.
		for (size_t i = 0; i < node.children.size(); i++)
			if (TreeNamesEntity(*node.children[i], scope, name))
				return true;
		break;
	case SN_VARIABLE:
		if (node.type && IsReferenceType(node.type))
			for (size_t i = 0; i < node.children.size(); i++)
				if (TreeNamesEntity(*node.children[i], scope, name))
					return true;
		break;
	case SN_FUNCTION_DEFINITION:
		// A reference-returning body may hand out an alias.
		if (node.type && node.type->kind == TK_FUNCTION &&
		    node.type->target && IsReferenceType(node.type->target) &&
		    TreeNamesEntity(node, scope, name))
			return true;
		break;
	default:
		break;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		if (TreeHasUnsafeUse(*node.children[i], scope, name))
			return true;
	return false;
}

}  // namespace

LowerProgram::LowerProgram()
	: has_main_(false), needs_eh_runtime_(false), lower_floor_(0)
{
	symbols_.insert("main");
}

void LowerProgram::AddUnit(const SemUnit& unit)
{
	units_.push_back(&unit);
	for (size_t i = 0; i < unit.items.size(); i++)
		CollectItem(*unit.items[i]);
	// unit.synthesized is the PA12 dump artifact; the real demand-driven
	// member definitions arrive through unit.deferred.
	for (size_t i = 0; i < unit.deferred.size(); i++)
		RegisterDeferred(*unit.deferred[i]);
	// PA18 14.7.2p8: an explicit instantiation definition emits every
	// instantiated member definition of its class unconditionally.
	for (size_t e = 0; e < unit.explicit_instantiations.size(); e++)
	{
		const NamedTypeInfo* entity = unit.explicit_instantiations[e];
		for (size_t i = 0; i < unit.deferred.size(); i++)
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
		if (!item.inline_def)
		{
			// An out-of-class constructor or destructor prints both
			// ABI entries.
			bool is_ctor = item.special == SF_CONSTRUCTOR ||
				item.special == SF_CONSTRUCTOR_BASE;
			MemberFunctionEntry(item.entity_scope, item.entity_name,
			                    item.type,
			                    is_ctor ? "C2" : "D2").used = true;
			MemberFunctionEntry(item.entity_scope, item.entity_name,
			                    item.type,
			                    is_ctor ? "C1" : "D1").used = true;
		}
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
	// Static member functions and hidden friends register as ordinary
	// entries; friend definitions emit unconditionally, static members
	// on demand.
	LowFunctionInfo& info = FunctionEntry(item.entity_scope,
	                                      item.entity_name, item.type,
	                                      item.fn_spec);
	if (!info.defined)
	{
		info.defined = true;
		info.weak = true;
		info.definition = &item;
		info.unwind_no = item.unwind_no;
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
	info.object_name = MangleVariableObjectName(scope, name);
	global_index_[key] = globals_.size();
	globals_.push_back(info);
	return globals_.back();
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
	info.object_name = MangleMemberFunctionObjectName(scope, name, type,
	                                                  special_code);
	// Complete and base entries are identical without virtual bases;
	// an emitted complete entry carries the base-entry alias.
	if (special_code == "C1")
		info.alias_object =
			MangleMemberFunctionObjectName(scope, name, type, "C2");
	else if (special_code == "D1")
		info.alias_object =
			MangleMemberFunctionObjectName(scope, name, type, "D2");
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
		// 7.1.2p4: inline definitions emit weak, on demand.
		if (item.inline_def)
			info.weak = true;
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
				if (!info.definition->synthesized)
					DemandFunction(info);
				else if (demanded_trees_.insert(
				             info.definition).second)
					DemandTreeCallees(*info.definition);
			}
		}
		DemandTreeCallees(child);
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

// --- rendering -------------------------------------------------------------

string LowerProgram::GlobalMetadata(const LowGlobalInfo& info) const
{
	string meta = info.weak ? "binding=weak"
		: info.internal ? "binding=internal" : "binding=strong";
	if (!info.object_name.empty())
		meta += ", object=" + info.object_name;
	if (info.is_thread_local)
		meta += ", storage=thread_local";
	return " [" + meta + "]";
}

string LowerProgram::RenderAddress(const LowerConst& value)
{
	string symbol;
	if (value.kind == LC_STRING)
		symbol = StringLiteralRef(*value.string_node);
	else if (value.entity_type->kind == TK_FUNCTION)
		symbol = FunctionRef(value.entity_scope, value.entity_name,
		                     value.entity_type);
	else
		symbol = GlobalRef(value.entity_scope, value.entity_name);
	string text = "addr " + symbol;
	if (value.kind == LC_ADDR && value.offset)
	{
		if (value.offset < 0)
			throw OutsideBoundary("negative address offset");
		text += " + " + to_string(value.offset);
	}
	return text;
}

// One structured data item; `is_zero_item` marks null pointers, which
// spell as zero fill.
string LowerProgram::RenderConstItem(const LowerConst& value,
                                     const TypePtr& type,
                                     bool& is_zero_item)
{
	is_zero_item = false;
	if (value.kind == LC_NULL)
	{
		is_zero_item = true;
		return "zero " + to_string(TypeSize(type));
	}
	if (value.kind == LC_INT)
		return LowerValueType(type) + " " +
			RenderConstValue(ConvertConstValue(value.ival,
			                                   ValueFund(type)));
	if (value.kind == LC_FLOAT)
		return LowerValueType(type) + " " + FloatInitToken(type, value);
	if (value.offset)
		throw OutsideBoundary("structured address item offset");
	return "ptr " + RenderAddress(value);
}

string LowerProgram::RenderScalarInit(const LowGlobalInfo& info)
{
	if (!info.node || info.node->children.empty() || info.dynamic_init)
		return "zero";
	LowerConst value;
	if (!EvaluateLowerConst(*info.node->children[0], value))
		throw OutsideBoundary("non-constant global initializer");
	switch (value.kind)
	{
	case LC_INT:
		return RenderConstValue(ConvertConstValue(value.ival,
		                                          ValueFund(info.type)));
	case LC_FLOAT:
		return FloatInitToken(info.type, value);
	case LC_NULL:
		// A null-pointer initializer spells the immediate zero.
		return "0";
	case LC_ADDR:
	case LC_STRING:
		return RenderAddress(value);
	}
	throw OutsideBoundary("global initializer");
}

string LowerProgram::RenderArrayItems(const LowGlobalInfo& info)
{
	const TypePtr& array = info.type;
	TypePtr element = RemoveTopCv(array->target);
	unsigned long long element_size = TypeSize(element);
	const SemNode* braced = 0;
	if (info.node && !info.node->children.empty())
	{
		braced = info.node->children[0].get();
		if (braced->kind != SN_BRACED_INIT_LIST)
			throw OutsideBoundary("global array initializer form");
	}
	size_t written = braced ? braced->children.size() : 0;
	string body;
	for (size_t i = 0; i < written; i++)
	{
		LowerConst value;
		if (!EvaluateLowerConst(*braced->children[i], value))
			throw OutsideBoundary("non-constant array element");
		bool is_zero_item = false;
		body += "  " + RenderConstItem(value, element, is_zero_item) +
			"\n";
	}
	if (array->bound > written)
		body += "  zero " +
			to_string((array->bound - written) * element_size) + "\n";
	return body;
}

string LowerProgram::RenderGlobal(const LowGlobalInfo& info)
{
	TypePtr inner = info.type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	if (RemoveTopCv(inner)->kind == TK_CLASS)
		// Class objects zero-fill statically; construction runs in
		// @__cppgm_init.
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n  zero " + to_string(TypeSize(info.type)) + "\n}";
	if (RemoveTopCv(info.type)->kind == TK_ENUM && info.folded_const)
		// An enumeration constant object: every read folds to the
		// recorded value, so the storage keeps the structured zero
		// image (the checked references pin this shape).
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n  zero " + to_string(TypeSize(info.type)) + "\n}";
	if (IsReferenceType(info.type))
		return "global @" + info.low_name + " : ptr" +
			GlobalMetadata(info) + " = " + RenderScalarInit(info);
	if (info.type->kind == TK_ARRAY)
		return "global @" + info.low_name + GlobalMetadata(info) +
			" = {\n" + RenderArrayItems(info) + "}";
	return "global @" + info.low_name + " : " +
		LowerValueType(info.type) + GlobalMetadata(info) + " = " +
		RenderScalarInit(info);
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
	// (witness: inherited-class-template-conversion-operator).
	if ((info.special_code == "C2" || info.special_code == "D2") &&
	    info.defined && info.definition && !info.definition->synthesized)
	{
		const ClassInfo* cls = MethodClass(info.type);
		bool comdat_pair = cls &&
			((cls->is_polymorphic && cls->dtor_virtual) ||
			 (cls->entity && cls->entity->spec_template &&
			  !cls->base));
		if (comdat_pair)
			DemandFunction(MemberFunctionEntry(
				info.scope, info.name, info.type,
				info.special_code == "C2" ? "C1" : "D1"));
	}
}

bool LowerProgram::BranchSpellsFnPointerAddress(const Scope* scope,
                                                const string& name)
{
	string key = QualifiedKey(scope, name);
	map<string, bool>::iterator cached = branch_folds_.find(key);
	if (cached != branch_folds_.end())
		return cached->second;
	bool fold = false;
	do
	{
		// Another unit could store through an extern declaration.
		if (units_.size() != 1)
			break;
		map<string, size_t>::iterator found = global_index_.find(key);
		if (found == global_index_.end())
			break;
		const LowGlobalInfo& info = globals_[found->second];
		// The object's only store must be the dynamic-init one, and it
		// must store a provably non-null constant address.
		if (!info.defined || !info.dynamic_init ||
		    info.is_thread_local || !info.node ||
		    info.node->children.size() != 1 ||
		    !InitIsNonNullAddress(*info.node->children[0]))
			break;
		// No call may run before that store (a branch reached from an
		// earlier initializer would read the zero image).
		bool earlier_call = false;
		for (size_t i = 0; i < found->second && !earlier_call; i++)
			earlier_call = GlobalInitRunsCall(globals_[i]);
		if (earlier_call)
			break;
		// Nothing anywhere may write to or alias the object (the
		// defining store above is synthesized separately and names
		// only the stored function).
		bool unsafe = false;
		for (size_t u = 0; u < units_.size() && !unsafe; u++)
		{
			const SemUnit& unit = *units_[u];
			for (size_t i = 0; i < unit.items.size() && !unsafe; i++)
				unsafe = TreeHasUnsafeUse(*unit.items[i], scope, name);
			for (size_t i = 0; i < unit.deferred.size() && !unsafe; i++)
				unsafe = TreeHasUnsafeUse(*unit.deferred[i], scope, name);
		}
		if (unsafe)
			break;
		fold = true;
	} while (false);
	branch_folds_[key] = fold;
	return fold;
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
				if (!info.defined || !info.body_text.empty())
					continue;
				bool friend_body = phase > 0 && info.scope &&
					info.scope->kind == SCOPE_NAMESPACE;
				if (info.weak && !info.used && !friend_body)
					continue;
				FunctionLowerer lowerer(*this, *info.definition, info);
				info.body_text = lowerer.Lower();
			}
		}
	}
}

void LowerProgram::LowerHelper(LowFunctionInfo& info,
                               const SemNode& definition)
{
	info.definition = &definition;
	info.defined = true;
	FunctionLowerer lowerer(*this, definition, info);
	info.body_text = lowerer.Lower();
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
		// PA18: a weak (instantiated static member) object does not by
		// itself anchor the init helper; a thread-local object
		// constructs behind its own first-use guard, not in the
		// program-wide init helper.
		bool tls_dynamic = info.is_thread_local && is_class;
		if (is_class && !info.weak && !tls_dynamic)
			any_class_object = true;
		const SemNode& item = *info.node;
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
	bool want_init = any_class_object || !init_def->children.empty();
	bool want_fini = !fini_def->children.empty();
	if (want_init)
	{
		LowFunctionInfo info;
		info.scope = 0;
		info.name = "__cppgm_init";
		info.type = init_def->type;
		info.low_name = UniqueSymbol("__cppgm_init");
		info.internal = true;
		info.role = "init";
		info.index = functions_.size();
		functions_.push_back(info);
		LowerHelper(functions_.back(), *init_def);
		helper_defs_.push_back(std::move(init_def));
	}
	if (want_fini)
	{
		LowFunctionInfo info;
		info.scope = 0;
		info.name = "__cppgm_fini";
		info.type = fini_def->type;
		info.low_name = UniqueSymbol("__cppgm_fini");
		info.internal = true;
		info.role = "fini";
		info.index = functions_.size();
		functions_.push_back(info);
		LowerHelper(functions_.back(), *fini_def);
		helper_defs_.push_back(std::move(fini_def));
	}
}

// Thread-local objects expose their access wrapper (the backend's
// TLS entry point).
void LowerProgram::AppendTlsWrapperDeclares(vector<string>& declares)
{
	for (size_t i = 0; i < globals_.size(); i++)
	{
		const LowGlobalInfo& info = globals_[i];
		if (!info.is_thread_local || (!info.defined && !info.used))
			continue;
		string object = info.object_name;
		if (object.compare(0, 2, "_Z") == 0)
			object = "_ZTW" + object.substr(2);
		string meta = info.internal ? "binding=internal"
		                            : "binding=strong";
		if (!object.empty())
			meta += ", object=" + object;
		declares.push_back(
			"declare function @" + info.low_name +
			"__tls_wrapper() -> ptr [" + meta +
			", tls_for=@" + info.low_name + "]");
	}
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
		if (info.defined || !info.used || info.deleted)
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

string LowerProgram::RenderFunctionDeclare(const LowFunctionInfo& info)
{
	string ret_text;
	bool indirect_result = LowerAbiReturn(info.type->target, ret_text);
	string params;
	size_t at = 0;
	if (indirect_result)
	{
		params = "%ret : ptr [pass=indirect_result]";
		at = 1;
	}
	for (size_t i = 0; i < info.type->parameters.size(); i++, at++)
	{
		const TypePtr& param = info.type->parameters[i];
		string param_text;
		string pass;
		LowerAbiParameter(param, param_text, pass);
		params += (at ? ", " : "") + string("%arg") + to_string(at) +
			" : " + param_text;
		if (!pass.empty())
			params += " [pass=" + pass + "]";
	}
	vector<string> meta;
	if (info.type->variadic)
		meta.push_back("arity=variadic");
	if (info.c_linkage)
		meta.push_back("linkage=c");
	meta.push_back(info.internal ? "binding=internal"
	                             : "binding=strong");
	if (!info.object_name.empty())
		meta.push_back("object=" + info.object_name);
	string metadata;
	for (size_t i = 0; i < meta.size(); i++)
		metadata += (i ? ", " : " [") + meta[i];
	metadata += "]";
	return "declare function @" + info.low_name + "(" + params +
		") -> " + ret_text + metadata;
}
