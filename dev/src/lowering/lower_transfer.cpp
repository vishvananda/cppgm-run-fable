#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// PA33 construction-transfer lowering: the raw storage transfer of a
// trivially copyable object and the -O1 call-site expansion of simple
// inline constructors. Split from lower_member.cpp (the general
// member/lifetime machinery stays there).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

TypePtr StripRef(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

// Whether evaluating this expression can have observable effects
// (calls, stores, allocation); pure address chains are elidable.
bool ExprHasSideEffects(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_CALL_EXPRESSION:
	case SN_ASSIGNMENT_EXPRESSION:
	case SN_CONSTRUCTOR_ACTION:
	case SN_DESTRUCTOR_ACTION:
	case SN_NEW_INIT:
	case SN_NEW_ARRAY:
	case SN_DELETE_EXPRESSION:
	case SN_DELETE_ARRAY:
	case SN_POSTFIX_EXPRESSION:
		return true;
	case SN_UNARY_EXPRESSION:
		if (node.op == OP_INC || node.op == OP_DEC)
			return true;
		break;
	default:
		break;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		if (ExprHasSideEffects(*node.children[i]))
			return true;
	return false;
}

}  // namespace

void FunctionLowerer::LowerTrivialCopyAction(const SemNode& action,
                                             const string& this_text)
{
	const SemNode& call = *action.children[0];
	if (!call.children.empty())
		program_.DemandTrivialCtorBody(*call.children[0]);
	size_t first_arg = action.ctor_addressed ? 2 : 1;
	string dst = this_text;
	// A member-addressed action (an aggregate item) owns its exact
	// subobject address; a caller-supplied destination names only the
	// enclosing object.
	bool member_addressed = first_arg == 2 &&
		(dst.empty() ||
		 call.children[1]->children[0]->kind == SN_MEMBER_EXPRESSION);
	if (member_addressed)
		dst = LowerAddressExpr(*call.children[1]->children[0]);
	else if (dst.empty())
		throw OutsideBoundary("trivial copy without a destination");
	const SemNode& source = *call.children[first_arg];
	if (source.kind == SN_CALL_EXPRESSION && source.type &&
	    !IsReferenceType(source.type) &&
	    RemoveTopCv(source.type)->kind == TK_CLASS)
	{
		// A prvalue call result transfers straight into the
		// destination (no intermediate temporary).
		MaterializeClassResult(source, "tmpobj", dst);
		return;
	}
	TypePtr cls_type = RemoveTopCv(action.type);
	// An empty object has no bytes to transfer (the PA15 convention):
	// a member-addressed action prints only the member address, while
	// an argument/temporary copy still evaluates the source lvalue.
	bool empty = cls_type->named->class_record &&
		cls_type->named->class_record->is_empty;
	if (empty && member_addressed)
	{
		// Only an effect-free source elides; a side-effecting source
		// lvalue (`e(*get())` in a mem-initializer) must evaluate.
		if (ExprHasSideEffects(source))
			LowerAddressExpr(source);
		return;
	}
	string src = LowerAddressExpr(source);
	// A derived source adjusts to the copied base subobject.
	TypePtr source_type = RemoveTopCv(StripRef(source.type));
	if (source_type->kind == TK_CLASS)
	{
		src = AdjustToBase(src, source_type->named, cls_type->named);
	}
	if (empty)
		return;
	Emit("copyobj " + to_string(TypeSize(cls_type)) + "x" +
	     to_string(TypeAlignment(cls_type)) + " " + src + ", " + dst);
}

// PA33 -O1: an inline constructor whose whole effect is storing
// literal values into scalar members (the mem-initializer form of a
// simple ctor) expands at the call site; its linkonce body is then
// never demanded. Anything else keeps the ordinary call.
bool FunctionLowerer::LowerSimpleInlineConstruction(const SemNode& action,
                                                    const SemNode& callee,
                                                    const string& this_text)
{
	if (this_text.empty() || action.value_zero_fill)
		return false;
	const SemNode& call = *action.children[0];
	size_t first_arg = action.ctor_addressed ? 2 : 1;
	if (call.children.size() > first_arg)
		return false;
	if (callee.special != SF_CONSTRUCTOR &&
	    callee.special != SF_CONSTRUCTOR_BASE)
		return false;
	LowFunctionInfo& info = program_.CalleeEntryInfo(callee);
	if (!info.definition || !info.weak)
		return false;
	const ClassInfo* cls = program_.MethodClass(callee.type);
	if (!cls || cls->is_polymorphic || !cls->direct_bases.empty() ||
	    cls->has_user_dtor)
		return false;
	const SemNode& def = *info.definition;
	vector<std::pair<const SemNode*, const SemNode*>> stores;
	for (size_t i = 0; i < def.children.size(); i++)
	{
		const SemNode& child = *def.children[i];
		if (child.kind == SN_PARAMETER)
			continue;
		if (child.kind == SN_COMPOUND_STATEMENT &&
		    child.children.empty())
			continue;
		if (child.kind != SN_EXPRESSION_STATEMENT ||
		    child.children.size() != 1)
			return false;
		const SemNode& assign = *child.children[0];
		if (assign.kind != SN_ASSIGNMENT_EXPRESSION ||
		    assign.children.size() != 2 || assign.member_ref ||
		    assign.is_bit_field)
			return false;
		const SemNode& member = *assign.children[0];
		const SemNode& value = *assign.children[1];
		if (member.kind != SN_MEMBER_EXPRESSION || member.is_bit_field ||
		    member.member_ref || member.base_hops != 0 ||
		    member.vbase_index >= 0)
			return false;
		TypePtr bare = RemoveTopCv(StripRef(member.type));
		if (bare->kind != TK_FUNDAMENTAL && bare->kind != TK_POINTER &&
		    bare->kind != TK_ENUM)
			return false;
		if (value.kind != SN_LITERAL || !value.has_value)
			return false;
		stores.push_back(std::make_pair(&member, &value));
	}
	for (size_t i = 0; i < stores.size(); i++)
	{
		const SemNode& member = *stores[i].first;
		TypePtr bare = RemoveTopCv(StripRef(member.type));
		string address = NewTemp();
		Emit(address + " = index i8 [projection=field] " + this_text +
		     ", " + to_string(member.member_offset));
		LowerValue value = LowerLiteralValue(*stores[i].second);
		Emit("store " + LowerValueType(bare) + " " + value.text + ", " +
		     address);
	}
	return true;
}
