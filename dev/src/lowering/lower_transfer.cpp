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

