#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// Call-argument reference binding, the GNU statement-expression value
// forms, and the inline float-classification builtin folds. The rest
// of the expression lowering lives in lower_expr.cpp.

namespace {

TypePtr StripRef(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

// The computation type of a node: declared type with references and
// top-level cv stripped.
TypePtr NodeType(const SemNode& node)
{
	return RemoveTopCv(StripRef(node.type));
}

TypePtr BoolType()
{
	return MakeFundamentalType(FT_BOOL);
}

}  // namespace

// PA29: a direct call to one of the lazily declared global-scope
// float-classification builtins (sem_binder's ResolveBuiltinFunction);
// no runtime definition exists, so the lowering expands them inline.
bool FunctionLowerer::IsFloatBuiltinCall(const SemNode& node) const
{
	if (node.kind != SN_CALL_EXPRESSION || node.children.size() != 2 ||
	    node.children[0]->kind != SN_CALLEE)
		return false;
	const SemNode& callee = *node.children[0];
	if (callee.is_method || callee.special != SF_NONE ||
	    callee.vtable_slot >= 0 || !callee.entity_scope ||
	    callee.entity_scope->kind != SCOPE_NAMESPACE ||
	    callee.entity_scope->parent)
		return false;
	return callee.entity_name == "__builtin_isnan" ||
		callee.entity_name == "__builtin_nanl";
}

// __builtin_nanl: the tag argument evaluates (and is otherwise
// ignored); the default quiet NaN materializes as a `const f80 nanL`.
// __builtin_isnan: the operand's x87 storage image classifies in the
// integer domain - exponent all ones and not an infinity (the explicit
// integer bit set over an all-zero fraction) - so the query is a bit
// test, never a call that could recurse into the builtin itself.
LowerValue FunctionLowerer::LowerFloatBuiltin(const SemNode& node,
                                              const SemNode& callee)
{
	LowerValue result;
	if (callee.entity_name == "__builtin_nanl")
	{
		LowerValueExpr(*node.children[1]);
		result.type = NodeType(node);
		result.text = NewTemp();
		Emit(result.text + " = const f80 nanL");
		return result;
	}
	LowerValue operand = LowerValueExpr(*node.children[1]);
	string slot = AddMatSlot("fpclass_value", "f80");
	Emit("store f80 " + operand.text + ", $" + slot);
	string low = NewTemp();
	Emit(low + " = load i64 $" + slot);
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	string high_address = NewTemp();
	Emit(high_address + " = index i8 " + address + ", 8");
	string high = NewTemp();
	Emit(high + " = load u16 " + high_address);
	// The classification row (the reference presentation): the
	// exponent field, the 63 explicit-integer-excluded fraction bits,
	// and their zero/all-ones predicates. isnan reads the all-ones
	// exponent and the infinity predicates; the zero-exponent compare
	// belongs to the row.
	string exponent = NewTemp();
	Emit(exponent + " = binary and u16 " + high + ", 32767");
	string fraction = NewTemp();
	Emit(fraction + " = binary and i64 " + low + ", 9223372036854775807");
	string exponent_ones = NewTemp();
	Emit(exponent_ones + " = cmp eq u16 " + exponent + ", 32767");
	string exponent_zero = NewTemp();
	Emit(exponent_zero + " = cmp eq u16 " + exponent + ", 0");
	string fraction_zero = NewTemp();
	Emit(fraction_zero + " = cmp eq i64 " + fraction + ", 0");
	string integer_bit = NewTemp();
	Emit(integer_bit + " = cmp lt i64 " + low + ", 0");
	string infinite = NewTemp();
	Emit(infinite + " = binary and i64 " + integer_bit + ", " +
	     fraction_zero);
	string not_infinite = NewTemp();
	Emit(not_infinite + " = cmp eq i64 " + infinite + ", 0");
	string is_nan = NewTemp();
	Emit(is_nan + " = binary and i64 " + exponent_ones + ", " +
	     not_infinite);
	result.type = BoolType();
	result.text = NewTemp();
	Emit(result.text + " = convert trunc u8 i64 " + is_nan);
	return result;
}

// PA29 GNU statement expression: the leading statements run in their
// own cleanup scope; the trailing expression statement (when present)
// yields the expression's value.
const SemNode* FunctionLowerer::StatementExpressionTail(const SemNode& node)
{
	if (node.children.empty())
		return 0;
	const SemNode& compound = *node.children.back();
	if (compound.children.empty())
		return 0;
	const SemNode* last = compound.children.back().get();
	if (last->kind != SN_EXPRESSION_STATEMENT || last->children.empty())
		return 0;
	return last->children[0].get();
}

LowerValue FunctionLowerer::LowerStatementExpression(const SemNode& node,
                                                     bool as_value)
{
	LowerValue value;
	value.type = NodeType(node);
	if (node.children.empty())
		return value;
	const SemNode& compound = *node.children.back();
	const SemNode* tail = StatementExpressionTail(node);
	PushCleanupScope();
	size_t count = compound.children.size();
	for (size_t i = 0; i + (tail ? 1 : 0) < count; i++)
		LowerStatement(*compound.children[i]);
	if (tail)
	{
		if (as_value)
			value = LowerValueExpr(*tail);
		else
			LowerEffect(*tail);
	}
	PopCleanupScope(true);
	return value;
}

// PA29 GNU statement expression bound to a reference: the leading
// statements run, then the trailing value binds the referee.
string FunctionLowerer::LowerStatementExpressionReference(
	const SemNode& node, const TypePtr& referee)
{
	const SemNode* tail = StatementExpressionTail(node);
	if (!tail)
		throw std::runtime_error(
			"void statement expression bound to a reference");
	const SemNode& compound = *node.children.back();
	PushCleanupScope();
	for (size_t i = 0; i + 1 < compound.children.size(); i++)
		LowerStatement(*compound.children[i]);
	string address = LowerReferenceArgument(*tail, referee);
	PopCleanupScope(true);
	return address;
}

string FunctionLowerer::LowerReferenceArgument(const SemNode& node,
                                               const TypePtr& referee)
{
	TypePtr bare = RemoveTopCv(referee);
	if (node.kind == SN_BRACED_INIT_LIST && bare->kind == TK_ARRAY)
	{
		// PA24 8.5.4: a braced argument bound to a reference-to-array
		// parameter materializes its temporary array.
		string slot = AddMatSlot("argarr", LowerSlotType(bare));
		return LowerLocalArrayInit(node, slot, bare);
	}
	if (node.kind == SN_CLOSURE_INIT && bare->kind == TK_CLASS)
	{
		// PA24: a closure temporary bound to a reference parameter
		// materializes as an argument object.
		string slot = AddMatSlot("arg", LowerSlotType(bare));
		string address = NewTemp();
		Emit(address + " = addr $" + slot);
		LowerClosureInit(node, address);
		return address;
	}
	if (node.kind == SN_CONSTRUCTOR_ACTION && bare->kind == TK_CLASS)
	{
		// A class temporary binding a reference parameter: an exact
		// binding materializes as an argument object; a derived
		// temporary adjusts to the base referee (and keeps its own
		// dispatch region while live cleanups may unwind past it).
		TypePtr made = RemoveTopCv(node.type);
		bool exact = TypeEquals(made, bare);
		if (!exact && eh_armed_ && !eh_open_ && !in_cleanup_emission_ &&
		    !suppress_eh_regions_)
			OpenEhRegion();
		string address =
			MaterializeTemporary(node, exact ? "arg" : "tmpobj",
			                     true);
		if (!exact && made->kind == TK_CLASS)
		{
			address = AdjustToBase(address, made->named,
			                       bare->named);
		}
		return address;
	}
	if (node.kind == SN_STATEMENT_EXPRESSION)
		return LowerStatementExpressionReference(node, referee);
	TypePtr source = RemoveTopCv(StripRef(node.type));
	bool binds_directly = node.category != VC_PRVALUE &&
		(TypeEquals(source, bare) ||
		 (source->kind == TK_CLASS && bare->kind == TK_CLASS &&
		  BaseClassDistance(source->named, bare->named) >= 0));
	if (!binds_directly && node.category != VC_PRVALUE &&
	    source->kind == TK_CLASS && bare->kind == TK_CLASS &&
	    source->named && source->named->class_record)
	{
		// PA29: a shared virtual-base referee also binds the glvalue
		// directly; AdjustToBase projects the complete-object offset.
		unsigned long long complete = 0;
		if (CompleteObjectOffset(*source->named->class_record,
		                         bare->named, complete))
			binds_directly = true;
	}
	if (binds_directly)
	{
		// A reference-cast class call result has no address of its
		// own; the xvalue materializes its temporary (5.2.9p4).
		// Reference-returning calls keep their direct address.
		bool value_call = false;
		if (node.kind == SN_CALL_EXPRESSION &&
		    source->kind == TK_CLASS && !node.children.empty())
		{
			TypePtr through = NodeType(*node.children[0]);
			if (through->kind == TK_POINTER)
				through = through->target;
			if (through->kind == TK_FUNCTION)
				value_call = !IsReferenceType(through->target);
		}
		string address = value_call
			? MaterializeClassResult(node, "refcall", "")
			: LowerAddressExpr(node);
		if (source->kind == TK_CLASS && bare->kind == TK_CLASS)
		{
			address = AdjustToBase(address, source->named,
			                       bare->named);
		}
		return address;
	}
	if (source->kind == TK_CLASS && bare->kind == TK_CLASS)
	{
		// A class prvalue (call result, conditional) binding a
		// reference: materialize the result object. As with
		// constructor-action temporaries above, an exact binding is an
		// argument object; a derived temporary is a plain temporary
		// adjusted to the base referee.
		bool exact = TypeEquals(source, bare);
		string address =
			MaterializeClassResult(node, exact ? "arg" : "tmpobj", "");
		return AdjustToBase(address, source->named, bare->named);
	}
	// 8.5.3p5: materialize a temporary with the converted value. A
	// function source stores its plain address (the reference spells
	// no decay for the bound temporary).
	string slot = AddMatSlot("refarg", LowerSlotType(bare));
	string value = NodeType(node)->kind == TK_FUNCTION &&
	        bare->kind == TK_POINTER
		? LowerAddressExpr(node)
		: LowerValueAs(node, bare, LCC_INIT);
	Emit("store " + LowerValueType(bare) + " " + value + ", $" + slot);
	string temp = NewTemp();
	Emit(temp + " = addr $" + slot);
	return temp;
}
