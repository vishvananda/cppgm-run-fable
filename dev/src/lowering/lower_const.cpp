#include "lowering/lower_const.h"

#include "lowering/lower_types.h"
#include "sema/const_expr.h"

namespace {

// The integer fact type a constant of `type` computes in.
bool ConstFund(const TypePtr& type, EFundamentalType& fund)
{
	if (type->kind == TK_ENUM)
	{
		fund = type->named->enum_underlying;
		return true;
	}
	if (type->kind == TK_FUNDAMENTAL &&
	    IsIntegralFundamental(type->fundamental))
	{
		fund = type->fundamental;
		return true;
	}
	return false;
}

bool IsPointerResult(const TypePtr& type)
{
	return type->kind == TK_POINTER;
}

// The declaring-scope binding of an id-expression node.
const ScopeBinding* NodeBinding(const SemNode& node)
{
	if (!node.entity_scope)
		return 0;
	return FindOwnBinding(*node.entity_scope, node.entity_name);
}

bool EvaluateAddress(const SemNode& node, LowerConst& out);

// Integer binary operation in the value space of `fund`.
bool ApplyIntegerOp(ETokenType op, EFundamentalType fund,
                    const ConstValue& a, const ConstValue& b,
                    ConstValue& out)
{
	ConstValue lhs = ConvertConstValue(a, fund);
	ConstValue rhs = ConvertConstValue(b, fund);
	bool is_signed = IsSignedIntegralFundamental(fund);
	unsigned long long bits;
	switch (op)
	{
	case OP_PLUS: bits = lhs.bits + rhs.bits; break;
	case OP_MINUS: bits = lhs.bits - rhs.bits; break;
	case OP_STAR: bits = lhs.bits * rhs.bits; break;
	case OP_DIV:
		if (rhs.bits == 0)
			return false;
		bits = is_signed
			? (unsigned long long)((long long)lhs.bits /
			                       (long long)rhs.bits)
			: lhs.bits / rhs.bits;
		break;
	case OP_MOD:
		if (rhs.bits == 0)
			return false;
		bits = is_signed
			? (unsigned long long)((long long)lhs.bits %
			                       (long long)rhs.bits)
			: lhs.bits % rhs.bits;
		break;
	case OP_AMP: bits = lhs.bits & rhs.bits; break;
	case OP_BOR: bits = lhs.bits | rhs.bits; break;
	case OP_XOR: bits = lhs.bits ^ rhs.bits; break;
	case OP_LSHIFT: bits = lhs.bits << (rhs.bits & 63); break;
	case OP_RSHIFT:
		bits = is_signed ? (unsigned long long)((long long)lhs.bits >>
		                                        (rhs.bits & 63))
		                 : lhs.bits >> (rhs.bits & 63);
		break;
	default:
		return false;
	}
	out = ConvertConstValue(ConstValue(fund, bits), fund);
	return true;
}

bool EvaluateBinary(const SemNode& node, LowerConst& out)
{
	LowerConst lhs;
	LowerConst rhs;
	if (!EvaluateLowerConst(*node.children[0], lhs) ||
	    !EvaluateLowerConst(*node.children[1], rhs))
		return false;
	if (IsPointerResult(node.type))
	{
		// Pointer arithmetic: scale the integer side by the element
		// size and fold it into the address offset.
		LowerConst* address = lhs.kind == LC_ADDR ? &lhs : &rhs;
		LowerConst* index = lhs.kind == LC_ADDR ? &rhs : &lhs;
		if (address->kind != LC_ADDR || index->kind != LC_INT)
			return false;
		if (node.op != OP_PLUS &&
		    !(node.op == OP_MINUS && lhs.kind == LC_ADDR))
			return false;
		long long elements = (long long)index->ival.bits;
		long long bytes = elements *
			(long long)TypeSize(node.type->target);
		out = *address;
		out.offset += node.op == OP_PLUS ? bytes : -bytes;
		return true;
	}
	EFundamentalType fund;
	if (lhs.kind != LC_INT || rhs.kind != LC_INT ||
	    !ConstFund(node.type, fund))
		return false;
	out.kind = LC_INT;
	return ApplyIntegerOp(node.op, fund, lhs.ival, rhs.ival, out.ival);
}

bool EvaluateUnary(const SemNode& node, LowerConst& out)
{
	if (node.op == OP_AMP)
		return EvaluateAddress(*node.children[0], out);
	LowerConst operand;
	if (!EvaluateLowerConst(*node.children[0], operand) ||
	    operand.kind != LC_INT)
		return false;
	EFundamentalType fund;
	if (!ConstFund(node.type, fund))
		return false;
	ConstValue value = ConvertConstValue(operand.ival, fund);
	switch (node.op)
	{
	case OP_PLUS: break;
	case OP_MINUS: value.bits = 0 - value.bits; break;
	case OP_COMPL: value.bits = ~value.bits; break;
	case OP_LNOT: value.bits = value.bits == 0; break;
	default:
		return false;
	}
	out.kind = LC_INT;
	out.ival = ConvertConstValue(value, fund);
	return true;
}

// The address constant of an lvalue: a namespace-scope object, or an
// element of one through a constant subscript.
bool EvaluateAddress(const SemNode& node, LowerConst& out)
{
	if (node.kind == SN_ID_EXPRESSION)
	{
		const ScopeBinding* binding = NodeBinding(node);
		if (!binding || node.entity_scope->kind != SCOPE_NAMESPACE)
			return false;
		out.kind = LC_ADDR;
		out.entity_scope = node.entity_scope;
		out.entity_name = node.entity_name;
		out.entity_type = binding->type;
		out.offset = 0;
		return true;
	}
	if (node.kind == SN_SUBSCRIPT_EXPRESSION)
	{
		LowerConst base;
		LowerConst index;
		if (!EvaluateAddress(*node.children[0], base) ||
		    !EvaluateLowerConst(*node.children[1], index) ||
		    index.kind != LC_INT)
			return false;
		out = base;
		out.offset += (long long)index.ival.bits *
			(long long)TypeSize(node.type);
		return true;
	}
	return false;
}

}  // namespace

bool EvaluateLowerConst(const SemNode& node, LowerConst& out)
{
	if (node.null_pointer && (node.type->kind == TK_POINTER ||
	                          IsNullPtrType(node.type)))
	{
		out.kind = LC_NULL;
		return true;
	}
	if (node.is_string_literal)
	{
		out.kind = LC_STRING;
		out.string_node = &node;
		return true;
	}
	if (node.has_float)
	{
		// PA20: a folded floating constant stamped by the binder.
		out.kind = LC_FLOAT;
		out.float_token = node.float_token;
		out.float_type = RemoveTopCv(node.type);
		return true;
	}
	if (node.has_value)
	{
		out.kind = LC_INT;
		out.ival = node.value;
		return true;
	}
	switch (node.kind)
	{
	case SN_LITERAL:
		if (LowerFloatType(node.type))
		{
			out.kind = LC_FLOAT;
			out.float_token = node.token;
			out.float_type = node.type;
			return true;
		}
		return false;
	case SN_ID_EXPRESSION:
	{
		const ScopeBinding* binding = NodeBinding(node);
		if (!binding)
			return false;
		// A value that arrived with an instantiated out-of-class
		// member definition stays a load here: an unfolded read
		// reaching the lowering is a parse-scope read, which precedes
		// the definition's point of instantiation (14.6.4.1).
		if (binding->has_value && !binding->value_from_def)
		{
			out.kind = LC_INT;
			out.ival = binding->value;
			return true;
		}
		// Arrays and functions decay to constant addresses.
		if (binding->type->kind == TK_ARRAY ||
		    binding->type->kind == TK_FUNCTION)
			return EvaluateAddress(node, out);
		return false;
	}
	case SN_UNARY_EXPRESSION:
		return EvaluateUnary(node, out);
	case SN_BINARY_EXPRESSION:
		return EvaluateBinary(node, out);
	case SN_CAST_EXPRESSION:
	{
		LowerConst operand;
		if (!EvaluateLowerConst(*node.children[0], operand))
			return false;
		if (operand.kind == LC_INT)
		{
			EFundamentalType fund;
			if (!ConstFund(node.type, fund))
				return false;
			out.kind = LC_INT;
			out.ival = ConvertConstValue(operand.ival, fund);
			return true;
		}
		// Pointer-shaped constants pass through pointer casts.
		if (node.type->kind == TK_POINTER)
		{
			out = operand;
			return out.kind == LC_ADDR || out.kind == LC_NULL ||
				out.kind == LC_STRING;
		}
		return false;
	}
	default:
		return false;
	}
}
