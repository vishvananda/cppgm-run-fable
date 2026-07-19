#include "lowering/lower_function.h"

#include "lowering/lower_types.h"

using std::to_string;

// PA34 hosted builtin expansion: the GNU/Clang builtin families that
// lower to inline instruction sequences rather than runtime calls.
// The C-library-backed builtins (memcpy and friends) stay ordinary
// calls carrying host symbols (lower_unit.cpp); the float
// classification pair and the vararg cursor builtins keep their
// PA29/PA33 expansions in lower_expand.cpp.

namespace {

TypePtr StripRef(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

TypePtr NodeType(const SemNode& node)
{
	return RemoveTopCv(StripRef(node.type));
}

// The names this file expands (without the __builtin_ prefix; the
// C11 fence operators spell in full).
const char* const kHostedBuiltins[] = {
	"bswap16", "bswap32", "bswap64",
	"clz", "clzl", "clzll", "clzg",
	"ctz", "ctzl", "ctzll", "ctzg",
	"popcount", "popcountl", "popcountll", "popcountg",
	"expect", "assume_aligned", "prefetch", "unreachable",
	"inf", "inff", "infl", "huge_val", "huge_valf", "huge_vall",
	"nan", "nanf", "nans", "nansf", "nansl",
	"fpclassify", "flt_rounds", "is_constant_evaluated",
	"isinf", "isfinite", "isnormal", "signbit",
	"isgreater", "isgreaterequal", "isless", "islessequal",
	"islessgreater", "isunordered",
	"add_overflow", "sub_overflow", "mul_overflow",
	"operator_new", "operator_delete",
	0
};

// The expanded family member of a direct global-scope call, or "".
string HostedBuiltinName(const SemNode& node)
{
	if (node.kind != SN_CALL_EXPRESSION || node.children.empty() ||
	    node.children[0]->kind != SN_CALLEE)
		return string();
	const SemNode& callee = *node.children[0];
	if (callee.is_method || callee.special != SF_NONE ||
	    callee.vtable_slot >= 0 || !callee.entity_scope ||
	    callee.entity_scope->kind != SCOPE_NAMESPACE ||
	    callee.entity_scope->parent)
		return string();
	const string& name = callee.entity_name;
	if (name == "__c11_atomic_thread_fence" ||
	    name == "__c11_atomic_signal_fence")
		return name;
	// The atomic operator families (magic-typed callees).
	static const char* const kAtomicBuiltins[] = {
		"__c11_atomic_init", "__c11_atomic_load", "__c11_atomic_store",
		"__c11_atomic_exchange", "__c11_atomic_compare_exchange_strong",
		"__c11_atomic_compare_exchange_weak",
		"__atomic_load", "__atomic_load_n", "__atomic_store",
		"__atomic_store_n", "__atomic_exchange_n",
		"__atomic_add_fetch", "__atomic_sub_fetch",
		"__atomic_fetch_add", "__atomic_fetch_sub",
		"__c11_atomic_fetch_add", "__c11_atomic_fetch_sub",
		"__c11_atomic_fetch_and", "__c11_atomic_fetch_or",
		"__c11_atomic_fetch_xor",
		"__atomic_fetch_and", "__atomic_fetch_or", "__atomic_fetch_xor",
		"__atomic_and_fetch", "__atomic_or_fetch", "__atomic_xor_fetch",
		"__sync_lock_test_and_set", "__sync_lock_release", 0
	};
	for (const char* const* at = kAtomicBuiltins; *at; at++)
		if (name == *at)
			return name;
	if (name.compare(0, 10, "__builtin_") != 0)
		return string();
	string tail = name.substr(10);
	for (const char* const* at = kHostedBuiltins; *at; at++)
		if (tail == *at)
			return tail;
	return string();
}

}  // namespace

bool FunctionLowerer::IsHostedBuiltinCall(const SemNode& node) const
{
	string name = HostedBuiltinName(node);
	if (name.empty())
		return false;
	// The whole-program presentation keeps __builtin_unreachable as
	// its declared call (the PA15 boundary-metadata fixtures pin that
	// shape); hosted separate compilation erases it instead of
	// emitting an unresolvable host symbol.
	if (name == "unreachable" && !program_.SeparateCompilation())
		return false;
	return true;
}

// One lowered call argument, converted to its declared parameter
// type (the expanded calls skip the ordinary argument binding, and
// argument nodes carry their unconverted types).
LowerValue FunctionLowerer::BuiltinArgument(const SemNode& node,
                                            size_t index)
{
	const SemNode& callee = *node.children[0];
	LowerValue value = LowerValueExpr(*node.children[index]);
	if (callee.type && callee.type->kind == TK_FUNCTION &&
	    index - 1 < callee.type->parameters.size())
		value = ConvertValue(std::move(value),
		                     callee.type->parameters[index - 1],
		                     LCC_INIT);
	return value;
}

LowerValue FunctionLowerer::LowerHostedBuiltin(const SemNode& node)
{
	string name = HostedBuiltinName(node);
	if (name.compare(0, 5, "bswap") == 0)
		return LowerBuiltinBswap(node, name);
	if (name.compare(0, 3, "clz") == 0 ||
	    name.compare(0, 3, "ctz") == 0 ||
	    name.compare(0, 8, "popcount") == 0)
		return LowerBuiltinBitCount(node, name);
	if (name == "inf" || name == "inff" || name == "infl" ||
	    name.compare(0, 8, "huge_val") == 0 ||
	    name.compare(0, 3, "nan") == 0)
		return LowerBuiltinFloatConstant(node, name);
	if (name == "fpclassify")
		return LowerBuiltinFpclassify(node);
	if (name == "isinf" || name == "isfinite" || name == "isnormal" ||
	    name == "signbit" || name == "isgreater" ||
	    name == "isgreaterequal" || name == "isless" ||
	    name == "islessequal" || name == "islessgreater" ||
	    name == "isunordered")
		return LowerBuiltinFloatQuery(node, name);
	if (name == "add_overflow" || name == "sub_overflow" ||
	    name == "mul_overflow")
		return LowerBuiltinOverflow(node, name);
	if (name == "operator_new" || name == "operator_delete")
		return LowerBuiltinOperatorNewDelete(node, name);
	if (name.compare(0, 13, "__c11_atomic_") == 0 ||
	    name.compare(0, 9, "__atomic_") == 0 ||
	    name.compare(0, 7, "__sync_") == 0)
	{
		if (name != "__c11_atomic_thread_fence" &&
		    name != "__c11_atomic_signal_fence")
			return LowerBuiltinAtomic(node, name);
	}
	return LowerBuiltinAnnotation(node, name);
}

// The atomic operator families over the LowIR atomic instructions.
// Memory orders emit as constants (seq_cst when the order is a
// runtime value); x86 CAS is always strong.
LowerValue FunctionLowerer::LowerBuiltinAtomic(const SemNode& node,
                                               const string& name)
{
	const SemNode& callee = *node.children[0];
	TypePtr element;
	if (callee.type->parameters[0]->kind == TK_POINTER)
		element = RemoveTopCv(callee.type->parameters[0]->target);
	string spell = LowerValueType(element);
	LowerValue result;
	result.type = NodeType(node);
	// The trailing int arguments are memory orders (constant when
	// the caller spelled a macro; seq_cst otherwise).
	size_t last = node.children.size() - 1;
	long long order = 5;
	if (node.children[last]->has_value)
		order = (long long)node.children[last]->value.bits;
	if (order < 0 || order > 5)
		order = 5;
	string order_text = to_string(order);
	LowerValue pointer = BuiltinArgument(node, 1);
	if (name == "__c11_atomic_init")
	{
		LowerValue value = BuiltinArgument(node, 2);
		Emit("store " + spell + " " + value.text + ", " + pointer.text);
		return result;
	}
	if (name == "__c11_atomic_load" || name == "__atomic_load_n")
	{
		result.text = NewTemp();
		Emit(result.text + " = atomic_load " + spell + " " +
		     pointer.text + ", " + order_text);
		return result;
	}
	if (name == "__atomic_load")
	{
		LowerValue out = BuiltinArgument(node, 2);
		string loaded = NewTemp();
		Emit(loaded + " = atomic_load " + spell + " " + pointer.text +
		     ", " + order_text);
		Emit("store " + spell + " " + loaded + ", " + out.text);
		return result;
	}
	if (name == "__c11_atomic_store" || name == "__atomic_store_n")
	{
		LowerValue value = BuiltinArgument(node, 2);
		Emit("atomic_store " + spell + " " + value.text + ", " +
		     pointer.text + ", " + order_text);
		return result;
	}
	if (name == "__atomic_store")
	{
		LowerValue in = BuiltinArgument(node, 2);
		string value = NewTemp();
		Emit(value + " = load " + spell + " " + in.text);
		Emit("atomic_store " + spell + " " + value + ", " +
		     pointer.text + ", " + order_text);
		return result;
	}
	if (name == "__c11_atomic_exchange" || name == "__atomic_exchange_n" ||
	    name == "__sync_lock_test_and_set")
	{
		LowerValue value = BuiltinArgument(node, 2);
		if (name == "__sync_lock_test_and_set")
			order_text = "2";  // acquire
		result.text = NewTemp();
		Emit(result.text + " = atomic_exchange " + spell + " " +
		     pointer.text + ", " + value.text + ", " + order_text);
		return result;
	}
	if (name == "__sync_lock_release")
	{
		string zero = NewTemp();
		Emit(zero + " = const " + spell + " 0");
		Emit("atomic_store " + spell + " " + zero + ", " +
		     pointer.text + ", 3");  // release
		return result;
	}
	if (name == "__c11_atomic_compare_exchange_strong" ||
	    name == "__c11_atomic_compare_exchange_weak")
	{
		LowerValue expected = BuiltinArgument(node, 2);
		LowerValue desired = BuiltinArgument(node, 3);
		long long failure = 5;
		if (node.children[last]->has_value)
			failure = (long long)node.children[last]->value.bits;
		long long success = 5;
		if (node.children[last - 1]->has_value)
			success = (long long)node.children[last - 1]->value.bits;
		if (failure < 0 || failure > 5)
			failure = 5;
		if (success < 0 || success > 5)
			success = 5;
		string flag = NewTemp();
		Emit(flag + " = atomic_compare_exchange " + spell + " " +
		     pointer.text + ", " + expected.text + ", " + desired.text +
		     ", " + to_string(success) + ", " + to_string(failure));
		result.text = NewTemp();
		Emit(result.text + " = convert trunc u8 i64 " + flag);
		return result;
	}
	// Bitwise read-modify-write families: a compare-exchange loop
	// (x86 has no old-value-producing lock and/or/xor).
	if (name.find("_and") != string::npos ||
	    name.find("_or") != string::npos ||
	    name.find("_xor") != string::npos)
		return LowerBuiltinAtomicBitwise(node, name, spell,
		                                 pointer.text, order_text);
	// add/sub fetch families.
	LowerValue delta = BuiltinArgument(node, 2);
	string amount = delta.text;
	bool subtract = name == "__atomic_sub_fetch" ||
		name == "__atomic_fetch_sub" ||
		name == "__c11_atomic_fetch_sub";
	if (subtract)
	{
		string negated = NewTemp();
		Emit(negated + " = unary neg " + spell + " " + amount);
		amount = negated;
	}
	string updated = NewTemp();
	Emit(updated + " = atomic_add_fetch " + spell + " " + pointer.text +
	     ", " + amount + ", " + order_text);
	bool fetch_old = name == "__atomic_fetch_add" ||
		name == "__atomic_fetch_sub" ||
		name == "__c11_atomic_fetch_add" ||
		name == "__c11_atomic_fetch_sub";
	if (!fetch_old)
	{
		result.text = updated;
		return result;
	}
	result.text = NewTemp();
	Emit(result.text + " = binary sub " + spell + " " + updated + ", " +
	     amount);
	return result;
}

// The bitwise atomic read-modify-write forms: load, apply, publish
// through compare-exchange, retry on interference. The expected slot
// holds the observed value (compare-exchange refreshes it on
// failure), so the loop re-applies over the newest value.
LowerValue FunctionLowerer::LowerBuiltinAtomicBitwise(
	const SemNode& node, const string& name, const string& spell,
	const string& pointer, const string& order_text)
{
	LowerValue operand = BuiltinArgument(node, 2);
	string op = name.find("_and") != string::npos ? "and"
		: name.find("_xor") != string::npos ? "xor" : "or";
	// Loop-crossing values ride slots: MIR temps are block-local.
	string slot = AddMatSlot("atomicrmw", spell);
	string pointer_slot = AddMatSlot("atomicrmwp", "ptr");
	string operand_slot = AddMatSlot("atomicrmwv", spell);
	string initial = NewTemp();
	Emit(initial + " = atomic_load " + spell + " " + pointer + ", 0");
	Emit("store " + spell + " " + initial + ", $" + slot);
	Emit("store ptr " + pointer + ", $" + pointer_slot);
	Emit("store " + spell + " " + operand.text + ", $" + operand_slot);
	string retry = NewLabel("rmw_retry");
	string done = NewLabel("rmw_done");
	CloseInto(retry);
	string target = NewTemp();
	Emit(target + " = load ptr $" + pointer_slot);
	string observed = NewTemp();
	Emit(observed + " = load " + spell + " $" + slot);
	string amount = NewTemp();
	Emit(amount + " = load " + spell + " $" + operand_slot);
	string updated = NewTemp();
	Emit(updated + " = binary " + op + " " + spell + " " + observed +
	     ", " + amount);
	string expected_address = NewTemp();
	Emit(expected_address + " = addr $" + slot);
	string swapped = NewTemp();
	Emit(swapped + " = atomic_compare_exchange " + spell + " " +
	     target + ", " + expected_address + ", " + updated + ", " +
	     order_text + ", 0");
	ReferenceLabel(retry);
	ReferenceLabel(done);
	Terminate("branch " + swapped + ", ^" + done + ", ^" + retry);
	OpenBlock(done);
	LowerValue result;
	result.type = NodeType(node);
	bool fetch_old = name.compare(0, 19, "__c11_atomic_fetch_") == 0 ||
		name.compare(0, 15, "__atomic_fetch_") == 0;
	string old_value = NewTemp();
	Emit(old_value + " = load " + spell + " $" + slot);
	if (fetch_old)
	{
		result.text = old_value;
		return result;
	}
	string final_amount = NewTemp();
	Emit(final_amount + " = load " + spell + " $" + operand_slot);
	result.text = NewTemp();
	Emit(result.text + " = binary " + op + " " + spell + " " +
	     old_value + ", " + final_amount);
	return result;
}

// expect/assume_aligned reduce to their surviving operand; prefetch
// and unreachable evaluate operands only; the environment queries are
// translation-time constants; the C11 fences emit the LowIR fence
// with their constant memory order (conservatively seq_cst when the
// order is a runtime value).
LowerValue FunctionLowerer::LowerBuiltinAnnotation(const SemNode& node,
                                                   const string& name)
{
	LowerValue result;
	result.type = NodeType(node);
	if (name == "expect" || name == "assume_aligned")
	{
		LowerValue value = BuiltinArgument(node, 1);
		for (size_t i = 2; i < node.children.size(); i++)
			LowerEffect(*node.children[i]);
		value.type = result.type;
		return value;
	}
	if (name == "flt_rounds")
	{
		// 5.2.4.2.2: this implementation translates (and executes the
		// x87/SSE default) under round-to-nearest.
		result.text = NewTemp();
		Emit(result.text + " = const i32 1");
		return result;
	}
	if (name == "is_constant_evaluated")
	{
		result.text = NewTemp();
		Emit(result.text + " = const u8 0");
		return result;
	}
	if (name == "__c11_atomic_thread_fence" ||
	    name == "__c11_atomic_signal_fence")
	{
		const SemNode& order_node = *node.children[1];
		long long order = 5;
		if (order_node.has_value)
			order = static_cast<long long>(order_node.value.bits);
		else
			LowerEffect(order_node);
		if (order < 0 || order > 5)
			order = 5;
		Emit((name == "__c11_atomic_thread_fence"
		      ? string("atomic_thread_fence ")
		      : string("atomic_signal_fence ")) + to_string(order));
		return result;
	}
	// prefetch / unreachable: operand evaluation only.
	for (size_t i = 1; i < node.children.size(); i++)
		LowerEffect(*node.children[i]);
	return result;
}

LowerValue FunctionLowerer::LowerBuiltinBswap(const SemNode& node,
                                              const string& name)
{
	LowerValue operand = BuiltinArgument(node, 1);
	string spell = LowerValueType(operand.type);
	LowerValue result;
	result.type = NodeType(node);
	result.text = NewTemp();
	if (name == "bswap16")
	{
		// x86 bswap is 32/64-bit only; the 16-bit form is a rotate.
		string high = NewTemp();
		Emit(high + " = binary ushr " + spell + " " + operand.text +
		     ", 8");
		string low = NewTemp();
		Emit(low + " = binary shl " + spell + " " + operand.text +
		     ", 8");
		Emit(result.text + " = binary or " + spell + " " + high + ", " +
		     low);
		return result;
	}
	Emit(result.text + " = unary bswap " + spell + " " + operand.text);
	return result;
}

// The operand widened to a 64-bit zero-extended pattern.
string FunctionLowerer::BuiltinBitPattern64(const LowerValue& operand)
{
	string spell = LowerValueType(operand.type);
	if (spell == "i64")
		return operand.text;
	string wide = NewTemp();
	Emit(wide + " = convert zext i64 " + spell + " " + operand.text);
	return wide;
}

// SWAR population count of a 64-bit pattern.
string FunctionLowerer::BuiltinPopcount64(const string& pattern)
{
	string t1 = NewTemp();
	Emit(t1 + " = binary ushr i64 " + pattern + ", 1");
	string t2 = NewTemp();
	Emit(t2 + " = binary and i64 " + t1 + ", 6148914691236517205");
	string t3 = NewTemp();
	Emit(t3 + " = binary sub i64 " + pattern + ", " + t2);
	string t4 = NewTemp();
	Emit(t4 + " = binary and i64 " + t3 + ", 3689348814741910323");
	string t5 = NewTemp();
	Emit(t5 + " = binary ushr i64 " + t3 + ", 2");
	string t6 = NewTemp();
	Emit(t6 + " = binary and i64 " + t5 + ", 3689348814741910323");
	string t7 = NewTemp();
	Emit(t7 + " = binary add i64 " + t4 + ", " + t6);
	string t8 = NewTemp();
	Emit(t8 + " = binary ushr i64 " + t7 + ", 4");
	string t9 = NewTemp();
	Emit(t9 + " = binary add i64 " + t7 + ", " + t8);
	string t10 = NewTemp();
	Emit(t10 + " = binary and i64 " + t9 + ", 1085102592571150095");
	string t11 = NewTemp();
	Emit(t11 + " = binary mul i64 " + t10 + ", 72340172838076673");
	string count = NewTemp();
	Emit(count + " = binary ushr i64 " + t11 + ", 56");
	return count;
}

// clz/ctz/popcount over the operand's own width; the g forms take an
// optional explicit zero answer (the fixed-width forms are undefined
// at zero, where this expansion yields the width).
LowerValue FunctionLowerer::LowerBuiltinBitCount(const SemNode& node,
                                                 const string& name)
{
	LowerValue operand = BuiltinArgument(node, 1);
	unsigned width = static_cast<unsigned>(
		LowerValueWidth(operand.type) * 8);
	string pattern = BuiltinBitPattern64(operand);
	string count;
	if (name.compare(0, 8, "popcount") == 0)
		count = BuiltinPopcount64(pattern);
	else if (name.compare(0, 3, "clz") == 0)
	{
		string smeared = pattern;
		for (unsigned shift = 1; shift < width; shift *= 2)
		{
			string part = NewTemp();
			Emit(part + " = binary ushr i64 " + smeared + ", " +
			     to_string(shift));
			string merged = NewTemp();
			Emit(merged + " = binary or i64 " + smeared + ", " + part);
			smeared = merged;
		}
		string length = BuiltinPopcount64(smeared);
		count = NewTemp();
		string width_text = NewTemp();
		Emit(width_text + " = const i64 " + to_string(width));
		Emit(count + " = binary sub i64 " + width_text + ", " + length);
	}
	else
	{
		string minus1 = NewTemp();
		Emit(minus1 + " = binary sub i64 " + pattern + ", 1");
		string inverted = NewTemp();
		Emit(inverted + " = unary bitnot i64 " + pattern);
		string below = NewTemp();
		Emit(below + " = binary and i64 " + inverted + ", " + minus1);
		if (width < 64)
		{
			string masked = NewTemp();
			Emit(masked + " = binary and i64 " + below + ", " +
			     to_string((1ull << width) - 1));
			below = masked;
		}
		count = BuiltinPopcount64(below);
	}
	if (node.children.size() > 2)
	{
		// g-form zero answer: select arithmetically (predicates are
		// 0/1 values).
		LowerValue fallback = BuiltinArgument(node, 2);
		string fallback64 = NewTemp();
		Emit(fallback64 + " = convert sext i64 " +
		     LowerValueType(fallback.type) + " " + fallback.text);
		string is_zero = NewTemp();
		Emit(is_zero + " = cmp eq i64 " + pattern + ", 0");
		string nonzero = NewTemp();
		Emit(nonzero + " = cmp eq i64 " + is_zero + ", 0");
		string from_fallback = NewTemp();
		Emit(from_fallback + " = binary mul i64 " + is_zero + ", " +
		     fallback64);
		string from_count = NewTemp();
		Emit(from_count + " = binary mul i64 " + nonzero + ", " + count);
		string merged = NewTemp();
		Emit(merged + " = binary add i64 " + from_fallback + ", " +
		     from_count);
		count = merged;
	}
	LowerValue result;
	result.type = NodeType(node);
	result.text = NewTemp();
	Emit(result.text + " = convert trunc i32 i64 " + count);
	return result;
}

// A float constant materialized from its storage image (LowIR has no
// inf/snan literal spellings). f32/f64 build from one integer store;
// f80 fills the mantissa word and the exponent half separately.
LowerValue FunctionLowerer::BuiltinFloatImage(const TypePtr& type,
                                              unsigned long long low,
                                              unsigned exponent)
{
	string spell = LowerValueType(type);
	string slot = AddMatSlot("fimage", spell);
	if (spell == "f80")
	{
		string mantissa = NewTemp();
		string one = NewTemp();
		Emit(one + " = const i64 1");
		Emit(mantissa + " = binary shl i64 " + one + ", 63");
		if (low)
		{
			// The signaling mantissa: integer bit plus bit 61 (quiet
			// bit 62 clear).
			string extra = NewTemp();
			Emit(extra + " = binary shl i64 " + one + ", 61");
			string merged = NewTemp();
			Emit(merged + " = binary or i64 " + mantissa + ", " + extra);
			mantissa = merged;
		}
		Emit("store i64 " + mantissa + ", $" + slot);
		string address = NewTemp();
		Emit(address + " = addr $" + slot);
		string high_address = NewTemp();
		Emit(high_address + " = index i8 " + address + ", 8");
		string high = NewTemp();
		Emit(high + " = const u16 " + to_string(exponent));
		Emit("store u16 " + high + ", " + high_address);
	}
	else
	{
		string int_spell = spell == "f32" ? "u32" : "i64";
		string bits = NewTemp();
		Emit(bits + " = const " + int_spell + " " + to_string(low));
		Emit("store " + int_spell + " " + bits + ", $" + slot);
	}
	LowerValue result;
	result.type = type;
	result.text = NewTemp();
	Emit(result.text + " = load " + spell + " $" + slot);
	return result;
}

LowerValue FunctionLowerer::LowerBuiltinFloatConstant(const SemNode& node,
                                                      const string& name)
{
	// The nan-family tag argument evaluates (and is otherwise
	// ignored: the default quiet/signaling payload results).
	for (size_t i = 1; i < node.children.size(); i++)
		LowerEffect(*node.children[i]);
	TypePtr type = NodeType(node);
	string spell = LowerValueType(type);
	bool signaling = name.compare(0, 4, "nans") == 0;
	bool quiet_nan = !signaling && name.compare(0, 3, "nan") == 0;
	if (spell == "f80")
		return BuiltinFloatImage(type, signaling ? 1 : 0,
		                         quiet_nan ? 0 : 32767);
	if (quiet_nan)
		return BuiltinFloatImage(type, spell == "f32"
		                         ? 2143289344ull
		                         : 9221120237041090560ull, 0);
	if (signaling)
		return BuiltinFloatImage(type, spell == "f32"
		                         ? 2141192192ull
		                         : 9219994337134247936ull, 0);
	return BuiltinFloatImage(type, spell == "f32"
	                         ? 2139095040ull
	                         : 9218868437227405312ull, 0);
}

// The five fpclassify predicates (nan/inf/normal/subnormal/zero as
// 0/1 values) of one operand image, classified in its own format.
void FunctionLowerer::BuiltinFloatClassRow(const LowerValue& operand,
                                           string preds[5])
{
	string spell = LowerValueType(operand.type);
	string slot = AddMatSlot("fpclassify", spell);
	Emit("store " + spell + " " + operand.text + ", $" + slot);
	string exp_zero, exp_ones, frac_zero, frac_nonzero;
	if (spell == "f80")
	{
		string low = NewTemp();
		Emit(low + " = load i64 $" + slot);
		string address = NewTemp();
		Emit(address + " = addr $" + slot);
		string high_address = NewTemp();
		Emit(high_address + " = index i8 " + address + ", 8");
		string high = NewTemp();
		Emit(high + " = load u16 " + high_address);
		string exponent = NewTemp();
		Emit(exponent + " = binary and u16 " + high + ", 32767");
		exp_zero = NewTemp();
		Emit(exp_zero + " = cmp eq u16 " + exponent + ", 0");
		exp_ones = NewTemp();
		Emit(exp_ones + " = cmp eq u16 " + exponent + ", 32767");
		// The explicit integer bit rides the mantissa word: infinity
		// is the integer bit over a zero fraction.
		string one = NewTemp();
		Emit(one + " = const i64 1");
		string int_bit = NewTemp();
		Emit(int_bit + " = binary shl i64 " + one + ", 63");
		frac_zero = NewTemp();
		Emit(frac_zero + " = cmp eq i64 " + low + ", " + int_bit);
		string all_zero = NewTemp();
		Emit(all_zero + " = cmp eq i64 " + low + ", 0");
		// Zero/subnormal classify on the whole mantissa word.
		string sub_pattern = NewTemp();
		Emit(sub_pattern + " = cmp eq i64 " + all_zero + ", 0");
		preds[4] = NewTemp();
		Emit(preds[4] + " = binary and i64 " + exp_zero + ", " +
		     all_zero);
		preds[3] = NewTemp();
		Emit(preds[3] + " = binary and i64 " + exp_zero + ", " +
		     sub_pattern);
	}
	else
	{
		bool single = spell == "f32";
		string int_spell = single ? "u32" : "i64";
		string exp_mask = single ? "2139095040" : "9218868437227405312";
		string frac_mask = single ? "8388607" : "4503599627370495";
		string bits = NewTemp();
		Emit(bits + " = load " + int_spell + " $" + slot);
		string exponent = NewTemp();
		Emit(exponent + " = binary and " + int_spell + " " + bits +
		     ", " + exp_mask);
		string fraction = NewTemp();
		Emit(fraction + " = binary and " + int_spell + " " + bits +
		     ", " + frac_mask);
		exp_zero = NewTemp();
		Emit(exp_zero + " = cmp eq " + int_spell + " " + exponent +
		     ", 0");
		exp_ones = NewTemp();
		Emit(exp_ones + " = cmp eq " + int_spell + " " + exponent +
		     ", " + exp_mask);
		frac_zero = NewTemp();
		Emit(frac_zero + " = cmp eq " + int_spell + " " + fraction +
		     ", 0");
		frac_nonzero = NewTemp();
		Emit(frac_nonzero + " = cmp eq i64 " + frac_zero + ", 0");
		preds[4] = NewTemp();
		Emit(preds[4] + " = binary and i64 " + exp_zero + ", " +
		     frac_zero);
		preds[3] = NewTemp();
		Emit(preds[3] + " = binary and i64 " + exp_zero + ", " +
		     frac_nonzero);
	}
	if (frac_nonzero.empty())
	{
		frac_nonzero = NewTemp();
		Emit(frac_nonzero + " = cmp eq i64 " + frac_zero + ", 0");
	}
	preds[1] = NewTemp();
	Emit(preds[1] + " = binary and i64 " + exp_ones + ", " + frac_zero);
	preds[0] = NewTemp();
	Emit(preds[0] + " = binary and i64 " + exp_ones + ", " +
	     frac_nonzero);
	string special_a = NewTemp();
	Emit(special_a + " = binary or i64 " + preds[0] + ", " + preds[1]);
	string special_b = NewTemp();
	Emit(special_b + " = binary or i64 " + preds[3] + ", " + preds[4]);
	string special = NewTemp();
	Emit(special + " = binary or i64 " + special_a + ", " + special_b);
	preds[2] = NewTemp();
	Emit(preds[2] + " = cmp eq i64 " + special + ", 0");
}

// __builtin_fpclassify(nan, inf, normal, subnormal, zero, value):
// the class values select through the 0/1 predicate row.
LowerValue FunctionLowerer::LowerBuiltinFpclassify(const SemNode& node)
{
	string class_values[5];
	for (size_t i = 0; i < 5; i++)
	{
		LowerValue value = BuiltinArgument(node, 1 + i);
		class_values[i] = NewTemp();
		Emit(class_values[i] + " = convert sext i64 " +
		     LowerValueType(value.type) + " " + value.text);
	}
	LowerValue operand = BuiltinArgument(node, 6);
	string preds[5];
	BuiltinFloatClassRow(operand, preds);
	string sum;
	for (size_t i = 0; i < 5; i++)
	{
		string part = NewTemp();
		Emit(part + " = binary mul i64 " + class_values[i] + ", " +
		     preds[i]);
		if (sum.empty())
			sum = part;
		else
		{
			string merged = NewTemp();
			Emit(merged + " = binary add i64 " + sum + ", " + part);
			sum = merged;
		}
	}
	LowerValue result;
	result.type = NodeType(node);
	result.text = NewTemp();
	Emit(result.text + " = convert trunc i32 i64 " + sum);
	return result;
}

// The float classification/comparison queries: bit tests on the f80
// image (the classification row) and unordered-aware compares (an
// ordered predicate is false on NaN operands; isunordered reads the
// self-inequality of each operand).
LowerValue FunctionLowerer::LowerBuiltinFloatQuery(const SemNode& node,
                                                   const string& name)
{
	LowerValue result;
	result.type = NodeType(node);
	string flag;
	if (node.children.size() > 2)
	{
		LowerValue lhs = BuiltinArgument(node, 1);
		LowerValue rhs = BuiltinArgument(node, 2);
		if (name == "isunordered")
		{
			string lhs_nan = NewTemp();
			Emit(lhs_nan + " = cmp ne f80 " + lhs.text + ", " +
			     lhs.text);
			string rhs_nan = NewTemp();
			Emit(rhs_nan + " = cmp ne f80 " + rhs.text + ", " +
			     rhs.text);
			flag = NewTemp();
			Emit(flag + " = binary or i64 " + lhs_nan + ", " + rhs_nan);
		}
		else if (name == "islessgreater")
		{
			string below = NewTemp();
			Emit(below + " = cmp lt f80 " + lhs.text + ", " + rhs.text);
			string above = NewTemp();
			Emit(above + " = cmp gt f80 " + lhs.text + ", " + rhs.text);
			flag = NewTemp();
			Emit(flag + " = binary or i64 " + below + ", " + above);
		}
		else
		{
			string pred = name == "isgreater" ? "gt"
				: name == "isgreaterequal" ? "ge"
				: name == "isless" ? "lt" : "le";
			flag = NewTemp();
			Emit(flag + " = cmp " + pred + " f80 " + lhs.text + ", " +
			     rhs.text);
		}
	}
	else
	{
		LowerValue operand = BuiltinArgument(node, 1);
		if (name == "signbit")
		{
			string slot = AddMatSlot("signbit", "f80");
			Emit("store f80 " + operand.text + ", $" + slot);
			string address = NewTemp();
			Emit(address + " = addr $" + slot);
			string high_address = NewTemp();
			Emit(high_address + " = index i8 " + address + ", 8");
			string high = NewTemp();
			Emit(high + " = load u16 " + high_address);
			string sign = NewTemp();
			Emit(sign + " = binary and u16 " + high + ", 32768");
			flag = NewTemp();
			Emit(flag + " = cmp ne u16 " + sign + ", 0");
		}
		else
		{
			string preds[5];
			BuiltinFloatClassRow(operand, preds);
			if (name == "isinf")
				flag = preds[1];
			else if (name == "isnormal")
				flag = preds[2];
			else
			{
				string special = NewTemp();
				Emit(special + " = binary or i64 " + preds[0] + ", " +
				     preds[1]);
				flag = NewTemp();
				Emit(flag + " = cmp eq i64 " + special + ", 0");
			}
		}
	}
	result.text = NewTemp();
	Emit(result.text + " = convert trunc i32 i64 " + flag);
	return result;
}

// __builtin_{add,sub,mul}_overflow(a, b, out): compute exactly in
// 128 bits, truncate to the result width, and compare the
// re-extension against the exact value.
LowerValue FunctionLowerer::LowerBuiltinOverflow(const SemNode& node,
                                                 const string& name)
{
	LowerValue lhs = BuiltinArgument(node, 1);
	LowerValue rhs = BuiltinArgument(node, 2);
	LowerValue out = BuiltinArgument(node, 3);
	TypePtr bare = RemoveTopCv(lhs.type);
	string spell = LowerValueType(bare);
	string extend = LowerUnsignedOps(bare) ? "zext" : "sext";
	string wide_l = NewTemp();
	Emit(wide_l + " = convert " + extend + " i128 " + spell + " " +
	     lhs.text);
	string wide_r = NewTemp();
	Emit(wide_r + " = convert " + extend + " i128 " + spell + " " +
	     rhs.text);
	string op = name == "add_overflow" ? "add"
	            : name == "sub_overflow" ? "sub" : "mul";
	string wide = NewTemp();
	Emit(wide + " = binary " + op + " i128 " + wide_l + ", " + wide_r);
	string narrow = NewTemp();
	Emit(narrow + " = convert trunc " + spell + " i128 " + wide);
	string replay = NewTemp();
	Emit(replay + " = convert " + extend + " i128 " + spell + " " +
	     narrow);
	string overflow = NewTemp();
	Emit(overflow + " = cmp ne i128 " + wide + ", " + replay);
	Emit("store " + spell + " " + narrow + ", " + out.text);
	LowerValue result;
	result.type = NodeType(node);
	result.text = NewTemp();
	Emit(result.text + " = convert trunc u8 i64 " + overflow);
	return result;
}

// __builtin_operator_new/__builtin_operator_delete: direct calls to
// the global allocation functions by their host ABI spellings (the
// align_val_t overloads select on the trailing enum argument).
LowerValue FunctionLowerer::LowerBuiltinOperatorNewDelete(
	const SemNode& node, const string& name)
{
	vector<LowerValue> args;
	bool aligned = false;
	bool sized = false;
	for (size_t i = 1; i < node.children.size(); i++)
	{
		args.push_back(BuiltinArgument(node, i));
		if (i >= 2)
		{
			if (RemoveTopCv(StripRef(args.back().type))->kind == TK_ENUM)
				aligned = true;
			else
				sized = true;
		}
	}
	LowerValue result;
	result.type = NodeType(node);
	string ref;
	if (name == "operator_new")
	{
		ref = aligned
			? program_.ExternalRuntimeFnRef(
				"__builtin_operator_new_aligned",
				"(%arg0 : i64, %arg1 : i64) -> ptr [binding=strong, "
				"object=_ZnwmSt11align_val_t]")
			: program_.ExternalRuntimeFnRef(
				"__builtin_operator_new",
				"(%arg0 : i64) -> ptr [binding=strong, object=_Znwm]");
		result.text = NewTemp();
		string call = result.text + " = call ptr " + ref + "(" +
			args[0].text;
		for (size_t i = 1; i < args.size(); i++)
			call += ", " + args[i].text;
		Emit(call + ")");
		return result;
	}
	const char* key;
	const char* meta;
	if (sized && aligned)
	{
		key = "__builtin_operator_delete_sized_aligned";
		meta = "(%arg0 : ptr, %arg1 : i64, %arg2 : i64) -> void "
			"[unwind=no, binding=strong, object=_ZdlPvmSt11align_val_t]";
	}
	else if (aligned)
	{
		key = "__builtin_operator_delete_aligned";
		meta = "(%arg0 : ptr, %arg1 : i64) -> void [unwind=no, "
			"binding=strong, object=_ZdlPvSt11align_val_t]";
	}
	else if (sized)
	{
		key = "__builtin_operator_delete_sized";
		meta = "(%arg0 : ptr, %arg1 : i64) -> void [unwind=no, "
			"binding=strong, object=_ZdlPvm]";
	}
	else
	{
		key = "__builtin_operator_delete";
		meta = "(%arg0 : ptr) -> void [unwind=no, binding=strong, "
			"object=_ZdlPv]";
	}
	ref = program_.ExternalRuntimeFnRef(key, meta);
	string call = "call void " + ref + "(" + args[0].text;
	for (size_t i = 1; i < args.size(); i++)
		call += ", " + args[i].text;
	Emit(call + ")");
	return result;
}
