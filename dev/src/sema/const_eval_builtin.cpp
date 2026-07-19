#include "sema/const_eval.h"

#include <stdexcept>

using std::runtime_error;

// PA34 constant evaluation of the integer builtin families. The
// callee is one of the lazily declared global builtins
// (sem_builtin.cpp) or a magic-analyzed bit builtin (clzg/ctzg/
// popcountg keep their operand's own type); the fold computes the
// value the lowering would otherwise expand inline.

namespace {

runtime_error NotConstant(const string& what)
{
	return runtime_error(what + " is not a constant expression");
}

// The width in value bits of one evaluated integer operand.
unsigned OperandBits(const EvalValue& value)
{
	TypePtr bare = value.type ? RemoveTopCv(value.type) : TypePtr();
	if (!bare || (bare->kind != TK_FUNDAMENTAL && bare->kind != TK_ENUM))
		throw NotConstant("builtin integer operand");
	return static_cast<unsigned>(TypeSize(bare) * 8);
}

unsigned long long MaskToWidth(unsigned long long bits, unsigned width)
{
	if (width >= 64)
		return bits;
	return bits & ((1ull << width) - 1);
}

unsigned BitLength(unsigned long long value)
{
	unsigned length = 0;
	while (value)
	{
		length++;
		value >>= 1;
	}
	return length;
}

unsigned Popcount(unsigned long long value)
{
	unsigned count = 0;
	while (value)
	{
		count += static_cast<unsigned>(value & 1);
		value >>= 1;
	}
	return count;
}

unsigned long long ByteSwap(unsigned long long value, unsigned width)
{
	unsigned long long swapped = 0;
	for (unsigned at = 0; at < width; at += 8)
		swapped = (swapped << 8) | ((value >> at) & 0xff);
	return swapped;
}

}  // namespace

bool ConstEvalEngine::TryEvalBuiltinCall(const SemNode& node,
                                         EvalValue& out)
{
	const SemNode& callee = *node.children[0];
	if (callee.is_method || callee.special != SF_NONE ||
	    callee.vtable_slot >= 0 || !callee.entity_scope ||
	    callee.entity_scope->kind != SCOPE_NAMESPACE ||
	    callee.entity_scope->parent)
		return false;
	const string& name = callee.entity_name;
	if (name.compare(0, 10, "__builtin_") != 0)
		return false;
	string op = name.substr(10);
	TypePtr result_type = node.type ? RemoveTopCv(node.type) : TypePtr();
	if (!result_type || result_type->kind != TK_FUNDAMENTAL)
		return false;
	out.kind = EvalValue::EV_INT;
	out.type = result_type;
	if (op == "is_constant_evaluated")
	{
		out.ival = ConstValue(FT_BOOL, 1);
		return true;
	}
	if (op == "expect")
	{
		EvalValue value = EvalRead(*node.children[1]);
		EvalRead(*node.children[2]);
		out.ival = ConstValue(FT_LONG_INT, value.ival.bits);
		return true;
	}
	bool is_bswap = op.compare(0, 5, "bswap") == 0;
	bool is_clz = op.compare(0, 3, "clz") == 0;
	bool is_ctz = op.compare(0, 3, "ctz") == 0;
	bool is_popcount = op.compare(0, 8, "popcount") == 0;
	if (!is_bswap && !is_clz && !is_ctz && !is_popcount)
		return false;
	if (node.children.size() < 2)
		throw NotConstant("builtin argument count");
	EvalValue operand = EvalRead(*node.children[1]);
	// The fixed-width forms count within their declared parameter
	// type (the argument's conversion is not a materialized node);
	// the g forms keep the operand's own width.
	if (callee.type && callee.type->kind == TK_FUNCTION &&
	    !callee.type->parameters.empty())
		operand = Convert(operand, callee.type->parameters[0]);
	if (operand.kind != EvalValue::EV_INT)
		throw NotConstant("builtin integer operand");
	unsigned width = OperandBits(operand);
	unsigned long long bits = MaskToWidth(operand.ival.bits, width);
	if (is_bswap)
	{
		out.ival = ConstValue(result_type->fundamental,
		                      ByteSwap(bits, width));
		return true;
	}
	if (is_popcount)
	{
		out.ival = ConstValue(FT_INT, Popcount(bits));
		return true;
	}
	// clz/ctz: a zero operand is undefined for the fixed-width forms
	// (rejected in constant evaluation, as the reference compilers
	// do); the g forms take an explicit zero answer.
	unsigned long long zero_answer = width;
	bool have_zero_answer = false;
	if (op == "clzg" || op == "ctzg")
	{
		if (node.children.size() > 2)
		{
			EvalValue fallback = EvalRead(*node.children[2]);
			if (fallback.kind != EvalValue::EV_INT)
				throw NotConstant("builtin zero-answer operand");
			zero_answer = fallback.ival.bits;
			have_zero_answer = true;
		}
	}
	if (bits == 0 && !have_zero_answer)
		throw NotConstant("bit scan of zero");
	if (bits == 0)
		out.ival = ConstValue(FT_INT, zero_answer);
	else if (is_clz)
		out.ival = ConstValue(FT_INT, width - BitLength(bits));
	else
		out.ival = ConstValue(FT_INT, Popcount(~bits & (bits - 1) &
		                                       MaskToWidth(~0ull, width)));
	return true;
}
