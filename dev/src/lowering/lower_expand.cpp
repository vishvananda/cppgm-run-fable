#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::to_string;

// Compiler-expanded expression forms: shapes with no direct call or
// single-instruction lowering. The float-classification builtins fold
// to inline bit tests (no runtime definition exists), GNU statement
// expressions run their bound compound in place, and 128-bit division
// expands to a restoring shift-subtract loop so separately compiled
// units need no support library.

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

// PA33: the vararg-cursor and stack-allocation builtins (sem_binder's
// ResolveBuiltinFunction); no runtime definition exists.
bool FunctionLowerer::IsVarargBuiltinCall(const SemNode& node) const
{
	if (node.kind != SN_CALL_EXPRESSION || node.children.size() < 2 ||
	    node.children[0]->kind != SN_CALLEE)
		return false;
	const SemNode& callee = *node.children[0];
	if (callee.is_method || callee.special != SF_NONE ||
	    callee.vtable_slot >= 0 || !callee.entity_scope ||
	    callee.entity_scope->kind != SCOPE_NAMESPACE ||
	    callee.entity_scope->parent)
		return false;
	return callee.entity_name == "__builtin_va_start" ||
		callee.entity_name == "__builtin_va_end" ||
		callee.entity_name == "__builtin_alloca";
}

// __builtin_va_end has no work on this ABI (the arguments still
// evaluate). __builtin_va_start and __builtin_alloca lower to
// role-annotated calls the backend expands in place: the register
// save area and the frame pointer are backend facts.
LowerValue FunctionLowerer::LowerVarargBuiltin(const SemNode& node,
                                               const SemNode& callee)
{
	LowerValue result;
	result.type = NodeType(node);
	if (callee.entity_name == "__builtin_va_end")
	{
		LowerValueExpr(*node.children[1]);
		return result;
	}
	if (callee.entity_name == "__builtin_alloca")
	{
		LowerValue size = LowerValueExpr(*node.children[1]);
		string ref = program_.ExternalRuntimeFnRef(
			"__builtin_alloca",
			"(%arg0 : i64) -> ptr [role=alloca, unwind=no, linkage=c, "
			"binding=strong, object=__builtin_alloca]");
		result.text = NewTemp();
		Emit(result.text + " = call ptr " + ref + "(" + size.text +
		     ")");
		return result;
	}
	// va_start(list, last): the cursor pointer fills from the variadic
	// frame facts; the last-named-parameter argument only evaluates.
	LowerValue list = LowerValueExpr(*node.children[1]);
	for (size_t i = 2; i < node.children.size(); i++)
		LowerEffect(*node.children[i]);
	string ref = program_.ExternalRuntimeFnRef(
		"__builtin_va_start",
		"(%arg0 : ptr) -> void [role=va_start, unwind=no, linkage=c, "
		"binding=strong, object=__builtin_va_start]");
	Emit("call void " + ref + "(" + list.text + ")");
	return result;
}

// __builtin_va_arg(list, T): the SysV cursor walk - registers first
// (gp_offset under 48, fp_offset under 176), the overflow area after,
// with the taken slot's cursor bumped in place. Expanded here as
// plain loads/stores on the cursor record; only va_start needs
// backend frame facts.
LowerValue FunctionLowerer::LowerVaArg(const SemNode& node)
{
	const SemNode& list = *node.children[0];
	string base;
	if (RemoveTopCv(StripRef(list.type))->kind == TK_ARRAY)
		base = LowerAddressExpr(list);
	else
		base = LowerValueExpr(list).text;
	TypePtr type = RemoveTopCv(node.typeid_operand);
	bool is_float = LowerFloatType(type);
	bool is_f80 = is_float && TypeSize(type) == 16;
	string address_slot = AddMatSlot("va_arg", "ptr");
	string overflow_address = NewTemp();
	Emit(overflow_address + " = index i8 " + base + ", 8");
	if (is_f80)
	{
		// The x87 class lives in the overflow area only (16 bytes).
		string overflow = NewTemp();
		Emit(overflow + " = load ptr " + overflow_address);
		Emit("store ptr " + overflow + ", $" + address_slot);
		string next = NewTemp();
		Emit(next + " = index i8 " + overflow + ", 16");
		Emit("store ptr " + next + ", " + overflow_address);
	}
	else
	{
		// The register class: u32 cursor at +0 (INTEGER) or +4 (SSE),
		// slot stride 8 or 16, register area limit 48 or 176.
		long long cursor_offset = is_float ? 4 : 0;
		long long limit = is_float ? 176 : 48;
		long long stride = is_float ? 16 : 8;
		string cursor_address = base;
		if (cursor_offset)
		{
			cursor_address = NewTemp();
			Emit(cursor_address + " = index i8 " + base + ", " +
			     to_string(cursor_offset));
		}
		string cursor = NewTemp();
		Emit(cursor + " = load u32 " + cursor_address);
		string in_regs = NewTemp();
		Emit(in_regs + " = cmp lt u32 " + cursor + ", " +
		     to_string(limit));
		string reg_label = NewLabel("va_arg_reg");
		string mem_label = NewLabel("va_arg_mem");
		string done_label = NewLabel("va_arg_done");
		ReferenceLabel(reg_label);
		ReferenceLabel(mem_label);
		ReferenceLabel(done_label);
		Terminate("branch " + in_regs + ", ^" + reg_label + ", ^" +
		          mem_label);
		OpenBlock(reg_label);
		string save_address = NewTemp();
		Emit(save_address + " = index i8 " + base + ", 16");
		string save = NewTemp();
		Emit(save + " = load ptr " + save_address);
		string wide_cursor = NewTemp();
		Emit(wide_cursor + " = convert zext i64 u32 " + cursor);
		string reg_address = NewTemp();
		Emit(reg_address + " = index i8 " + save + ", " + wide_cursor);
		Emit("store ptr " + reg_address + ", $" + address_slot);
		string bumped = NewTemp();
		Emit(bumped + " = binary add u32 " + cursor + ", " +
		     to_string(stride));
		Emit("store u32 " + bumped + ", " + cursor_address);
		Terminate("jump ^" + done_label);
		OpenBlock(mem_label);
		string overflow = NewTemp();
		Emit(overflow + " = load ptr " + overflow_address);
		Emit("store ptr " + overflow + ", $" + address_slot);
		string next = NewTemp();
		Emit(next + " = index i8 " + overflow + ", 8");
		Emit("store ptr " + next + ", " + overflow_address);
		Terminate("jump ^" + done_label);
		OpenBlock(done_label);
	}
	string address = NewTemp();
	Emit(address + " = load ptr $" + address_slot);
	LowerValue result;
	result.type = type;
	result.text = NewTemp();
	Emit(result.text + " = load " + LowerValueType(type) + " " +
	     address);
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
	// Sema types a tail-less statement expression void and rejects it
	// in value positions; reaching one here as a value is a desync,
	// not a shape to paper over with an empty value.
	if (node.children.empty())
	{
		if (as_value)
			throw std::runtime_error(
				"empty statement expression used as a value");
		return value;
	}
	const SemNode& compound = *node.children.back();
	const SemNode* tail = StatementExpressionTail(node);
	if (!tail && as_value)
		throw std::runtime_error(
			"void statement expression used as a value");
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

// A 128-bit division or remainder has no direct machine shape; the
// value expands in place to a restoring shift-subtract loop over the
// magnitude (with a branch-free sign fixup for the signed forms), so
// separately compiled units need no support library.
string FunctionLowerer::LowerWideDivMod(const string& op_name,
                                        const TypePtr& type,
                                        const string& lhs,
                                        const string& rhs)
{
	(void)type;
	bool is_signed = op_name == "div" || op_name == "mod";
	bool want_mod = op_name == "mod" || op_name == "umod";
	string n = lhs;
	string d = rhs;
	string sign_n;
	string sign_d;
	if (is_signed)
	{
		// abs(x) = (x ^ (x >> 127)) - (x >> 127), branch-free.
		sign_n = NewTemp();
		Emit(sign_n + " = binary shr i128 " + lhs + ", 127");
		string xn = NewTemp();
		Emit(xn + " = binary xor i128 " + lhs + ", " + sign_n);
		n = NewTemp();
		Emit(n + " = binary sub i128 " + xn + ", " + sign_n);
		sign_d = NewTemp();
		Emit(sign_d + " = binary shr i128 " + rhs + ", 127");
		string xd = NewTemp();
		Emit(xd + " = binary xor i128 " + rhs + ", " + sign_d);
		d = NewTemp();
		Emit(d + " = binary sub i128 " + xd + ", " + sign_d);
	}
	string n_slot = AddMatSlot("wdivn", "i128");
	string q_slot = AddMatSlot("wdivq", "i128");
	string r_slot = AddMatSlot("wdivr", "i128");
	string i_slot = AddMatSlot("wdivi", "i64");
	Emit("store i128 " + n + ", $" + n_slot);
	Emit("store i128 0, $" + q_slot);
	Emit("store i128 0, $" + r_slot);
	Emit("store i64 128, $" + i_slot);
	string head = NewLabel("wdiv_head");
	string body = NewLabel("wdiv_body");
	string done = NewLabel("wdiv_done");
	ReferenceLabel(head);
	Terminate("jump ^" + head);
	OpenBlock(head);
	string count = NewTemp();
	Emit(count + " = load i64 $" + i_slot);
	string finished = NewTemp();
	Emit(finished + " = cmp eq i64 " + count + ", 0");
	ReferenceLabel(done);
	ReferenceLabel(body);
	Terminate("branch " + finished + ", ^" + done + ", ^" + body);
	OpenBlock(body);
	// One restoring step: (r:n) shifts left one bit, and the divisor
	// subtracts from r under an all-ones mask when it fits.
	string r0 = NewTemp();
	Emit(r0 + " = load i128 $" + r_slot);
	string r1 = NewTemp();
	Emit(r1 + " = binary shl i128 " + r0 + ", 1");
	string n0 = NewTemp();
	Emit(n0 + " = load i128 $" + n_slot);
	string top = NewTemp();
	Emit(top + " = binary ushr i128 " + n0 + ", 127");
	string r2 = NewTemp();
	Emit(r2 + " = binary or i128 " + r1 + ", " + top);
	string n1 = NewTemp();
	Emit(n1 + " = binary shl i128 " + n0 + ", 1");
	Emit("store i128 " + n1 + ", $" + n_slot);
	string q0 = NewTemp();
	Emit(q0 + " = load i128 $" + q_slot);
	string q1 = NewTemp();
	Emit(q1 + " = binary shl i128 " + q0 + ", 1");
	string fits = NewTemp();
	Emit(fits + " = cmp uge i128 " + r2 + ", " + d);
	string fits_wide = NewTemp();
	Emit(fits_wide + " = convert zext i128 i64 " + fits);
	string mask = NewTemp();
	Emit(mask + " = binary sub i128 0, " + fits_wide);
	string d_masked = NewTemp();
	Emit(d_masked + " = binary and i128 " + d + ", " + mask);
	string r3 = NewTemp();
	Emit(r3 + " = binary sub i128 " + r2 + ", " + d_masked);
	Emit("store i128 " + r3 + ", $" + r_slot);
	string q2 = NewTemp();
	Emit(q2 + " = binary or i128 " + q1 + ", " + fits_wide);
	Emit("store i128 " + q2 + ", $" + q_slot);
	string next = NewTemp();
	Emit(next + " = binary sub i64 " + count + ", 1");
	Emit("store i64 " + next + ", $" + i_slot);
	ReferenceLabel(head);
	Terminate("jump ^" + head);
	OpenBlock(done);
	string magnitude = NewTemp();
	Emit(magnitude + " = load i128 $" + (want_mod ? r_slot : q_slot));
	if (!is_signed)
		return magnitude;
	// 5.6: the quotient truncates toward zero (its sign is the xor of
	// the operand signs); the remainder takes the dividend's sign.
	string sign = sign_n;
	if (!want_mod)
	{
		sign = NewTemp();
		Emit(sign + " = binary xor i128 " + sign_n + ", " + sign_d);
	}
	string flipped = NewTemp();
	Emit(flipped + " = binary xor i128 " + magnitude + ", " + sign);
	string result = NewTemp();
	Emit(result + " = binary sub i128 " + flipped + ", " + sign);
	return result;
}
