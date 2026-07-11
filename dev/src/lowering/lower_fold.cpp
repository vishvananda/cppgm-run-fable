#include "lowering/lower_program.h"

#include "lowering/lower_name.h"

// The branch-fold analysis behind BranchSpellsFnPointerAddress: whether
// a global function pointer provably holds one non-null constant
// address by the time any branch through it runs, so calls through it
// spell the direct callee (the reference presentation).

namespace {

// The program-wide identity key of a namespace-scope entity.
string FoldQualifiedKey(const Scope* scope, const string& name)
{
	return LowerScopeKey(scope) + name;
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

bool LowerProgram::BranchSpellsFnPointerAddress(const Scope* scope,
                                                const string& name)
{
	string key = FoldQualifiedKey(scope, name);
	map<string, bool>::iterator cached = branch_folds_.find(key);
	if (cached != branch_folds_.end())
		return cached->second;
	bool fold = false;
	do
	{
		// Another unit could store through an extern declaration. In
		// separate compilation the other units are never visible, so
		// the whole-program premise fails outright (PA29).
		if (separate_compilation_ || units_.size() != 1)
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
