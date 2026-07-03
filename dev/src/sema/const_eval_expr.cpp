#include "sema/const_eval.h"

#include <cstring>
#include <stdexcept>

#include "numeric_literals.h"
#include "sema/const_expr.h"

using std::runtime_error;

// Expression evaluation of the PA20 constant engine: rvalue reads,
// lvalue addresses, and initializer application over analyzed
// SemNode trees. Calls and statements live in const_eval_stmt.cpp.

namespace {

runtime_error NotConstant(const string& what)
{
	return runtime_error(what + " is not a constant expression");
}

bool IsLValueNode(const SemNode& node)
{
	return node.category == VC_LVALUE || node.category == VC_XVALUE;
}

TypePtr ValueTypeOf(const SemNode& node)
{
	TypePtr type = node.type;
	if (type && IsReferenceType(type))
		type = type->target;
	return type;
}

bool IsClassValued(const SemNode& node)
{
	TypePtr type = ValueTypeOf(node);
	return type && RemoveTopCv(type)->kind == TK_CLASS;
}

}  // namespace

ConstObjectPtr ConstEvalEngine::StringObject(const SemNode& node)
{
	map<const SemNode*, ConstObjectPtr>::const_iterator found =
		string_objects_.find(&node);
	if (found != string_objects_.end())
		return found->second;
	ConstObjectPtr object(new ConstObject());
	object->type = node.type;
	object->bytes.assign(node.string_bytes.begin(),
	                     node.string_bytes.end());
	string_objects_[&node] = object;
	return object;
}

EvalValue ConstEvalEngine::EvalLiteral(const SemNode& node)
{
	EvalValue value;
	value.type = RemoveTopCv(ValueTypeOf(node));
	// The null_pointer flag also rides on plain integer zeros; only a
	// pointer-typed node reads as the null pointer value.
	bool pointer_typed = value.type->kind == TK_POINTER ||
		value.type->kind == TK_MEMBER_POINTER ||
		(value.type->kind == TK_FUNDAMENTAL &&
		 value.type->fundamental == FT_NULLPTR_T);
	if (node.null_pointer && pointer_typed)
	{
		value.kind = EvalValue::EV_PTR;
		return value;
	}
	if (node.has_value)
	{
		value.kind = EvalValue::EV_INT;
		value.ival = node.value;
		return value;
	}
	if (node.is_string_literal)
	{
		// Decayed use of the literal object: pointer to element zero.
		value.kind = EvalValue::EV_PTR;
		value.ptr.object = StringObject(node);
		return value;
	}
	if (value.type->kind == TK_FUNDAMENTAL &&
	    IsFloatingFundamental(value.type->fundamental))
	{
		long double parsed = 0;
		if (!DecodeFloatLiteral(node.token, parsed))
			throw NotConstant("floating literal form");
		value.kind = EvalValue::EV_FLOAT;
		value.fval = parsed;
		return value;
	}
	throw NotConstant("literal form");
}

EvalValue ConstEvalEngine::EvalId(const SemNode& node)
{
	if (node.has_value)
	{
		EvalValue value;
		value.kind = EvalValue::EV_INT;
		value.ival = node.value;
		value.type = RemoveTopCv(ValueTypeOf(node));
		return value;
	}
	// `this` and ordinary frame/store objects read through the lvalue.
	if (node.entity_name == "this")
	{
		Frame* frame = CurrentFrame();
		if (frame)
			for (map<pair<const void*, string>,
			         ConstPointer>::const_iterator it =
			         frame->slots.begin();
			     it != frame->slots.end(); ++it)
				if (it->first.second == "this")
				{
					EvalValue value;
					value.kind = EvalValue::EV_PTR;
					value.ptr = it->second;
					value.type = RemoveTopCv(ValueTypeOf(node));
					return value;
				}
		throw NotConstant("this outside a constant call");
	}
	// A recorded constant binding folds without an object image
	// (enumerators, const integral variables, static members).
	if (node.entity_scope)
	{
		const ScopeBinding* binding =
			FindOwnBinding(*(const Scope*)node.entity_scope,
			               node.entity_name);
		if (binding && binding->has_value &&
		    !FindObject(node.entity_scope, node.entity_name))
		{
			Frame* frame = CurrentFrame();
			bool shadowed = frame &&
				frame->slots.count(pair<const void*, string>(
					node.entity_scope, node.entity_name));
			if (!shadowed)
			{
				EvalValue value;
				value.kind = EvalValue::EV_INT;
				value.ival = binding->value;
				value.type = RemoveTopCv(ValueTypeOf(node));
				return value;
			}
		}
	}
	ConstPointer address = EvalLValue(node);
	TypePtr type = ValueTypeOf(node);
	if (RemoveTopCv(type)->kind == TK_ARRAY)
	{
		EvalValue value;
		value.kind = EvalValue::EV_PTR;
		value.ptr = address;
		value.type = type;
		return value;
	}
	return ReadThrough(address, type);
}

ConstPointer ConstEvalEngine::MemberAddress(const SemNode& node)
{
	ConstPointer base = EvalLValue(*node.children[0]);
	base.offset += node.member_offset;
	if (node.is_bit_field)
		throw NotConstant("bit-field access");
	if (node.member_ref)
	{
		EvalValue through = ReadThrough(
			base, MakePointerType(ValueTypeOf(node), false, false));
		return through.ptr;
	}
	return base;
}

ConstPointer ConstEvalEngine::SubscriptAddress(const SemNode& node)
{
	const SemNode& base_node = *node.children[0];
	TypePtr base_type = RemoveTopCv(ValueTypeOf(base_node));
	ConstPointer base;
	if (base_type->kind == TK_ARRAY)
		base = EvalLValue(base_node);
	else
	{
		EvalValue pointer = EvalRead(base_node);
		if (pointer.kind != EvalValue::EV_PTR)
			throw NotConstant("subscript base");
		base = pointer.ptr;
	}
	EvalValue index = EvalRead(*node.children[1]);
	if (index.kind != EvalValue::EV_INT)
		throw NotConstant("subscript index");
	unsigned long long element =
		TypeSize(RemoveTopCv(ValueTypeOf(node)));
	base.offset += (long long)index.ival.bits * element;
	return base;
}

ConstPointer ConstEvalEngine::EvalLValue(const SemNode& node)
{
	Step();
	switch (node.kind)
	{
	case SN_ID_EXPRESSION:
	{
		Frame* frame = CurrentFrame();
		pair<const void*, string> key(node.entity_scope,
		                              node.entity_name);
		if (frame)
		{
			map<pair<const void*, string>, ConstPointer>::const_iterator
				found = frame->slots.find(key);
			if (found != frame->slots.end())
				return found->second;
		}
		ConstObjectPtr stored = FindObject(node.entity_scope,
		                                   node.entity_name);
		if (stored)
		{
			ConstPointer address;
			address.object = stored;
			return address;
		}
		// A folded constant read (enumerators, recorded const ints)
		// has no object; only its value is usable.
		if (node.has_value)
			throw NotConstant("address of a folded constant");
		// The address of a runtime global is an address constant.
		if (node.entity_scope &&
		    ((const Scope*)node.entity_scope)->kind == SCOPE_NAMESPACE)
		{
			ConstPointer address;
			address.sym_scope = node.entity_scope;
			address.sym_name = node.entity_name;
			return address;
		}
		throw NotConstant("read of a non-constant object");
	}
	case SN_MEMBER_EXPRESSION:
		return MemberAddress(node);
	case SN_SUBSCRIPT_EXPRESSION:
		return SubscriptAddress(node);
	case SN_LITERAL:
		if (node.is_string_literal)
		{
			ConstPointer address;
			address.object = StringObject(node);
			return address;
		}
		throw NotConstant("literal lvalue");
	case SN_UNARY_EXPRESSION:
		if (node.has_op && node.op == OP_STAR)
		{
			EvalValue pointer = EvalRead(*node.children[0]);
			if (pointer.kind != EvalValue::EV_PTR ||
			    pointer.ptr.IsNull())
				throw NotConstant("indirection operand");
			return pointer.ptr;
		}
		if (node.has_op && (node.op == OP_INC || node.op == OP_DEC))
		{
			EvalIncDec(node, true);
			return EvalLValue(*node.children[0]);
		}
		break;
	case SN_CAST_EXPRESSION:
		// Reference casts and derived-to-base adjustments keep the
		// address (bases sit at offset zero without multiple
		// inheritance).
		return EvalLValue(*node.children[0]);
	case SN_CALL_EXPRESSION:
	{
		if (IsClassValued(node) && node.category == VC_PRVALUE)
		{
			ConstObjectPtr result = Allocate(
				RemoveTopCv(ValueTypeOf(node)));
			ConstPointer dest;
			dest.object = result;
			EvalCall(node, &dest);
			return dest;
		}
		EvalValue value = EvalCall(node, 0);
		if (value.kind != EvalValue::EV_PTR || value.ptr.IsNull())
			throw NotConstant("call result lvalue");
		return value.ptr;
	}
	case SN_CONSTRUCTOR_ACTION:
	{
		// A materialized temporary: construct into fresh storage.
		ConstObjectPtr object = Allocate(RemoveTopCv(ValueTypeOf(node)));
		ConstPointer dest;
		dest.object = object;
		EvalConstructorAction(node, &dest, RemoveTopCv(ValueTypeOf(node)));
		return dest;
	}
	case SN_ASSIGNMENT_EXPRESSION:
	{
		EvalAssignment(node);
		return EvalLValue(*node.children[0]);
	}
	case SN_CONDITIONAL_EXPRESSION:
		return EvaluateBool(*node.children[0])
			? EvalLValue(*node.children[1])
			: EvalLValue(*node.children[2]);
	default:
		break;
	}
	throw NotConstant("lvalue form");
}

EvalValue ConstEvalEngine::EvalUnary(const SemNode& node)
{
	if (!node.has_op)
		throw NotConstant("unary form");
	switch (node.op)
	{
	case OP_AMP:
	{
		EvalValue value;
		value.kind = EvalValue::EV_PTR;
		value.ptr = EvalLValue(*node.children[0]);
		value.type = RemoveTopCv(ValueTypeOf(node));
		return value;
	}
	case OP_STAR:
	case OP_INC:
	case OP_DEC:
		return ReadThrough(EvalLValue(node), ValueTypeOf(node));
	case OP_LNOT:
	{
		EvalValue value;
		value.kind = EvalValue::EV_INT;
		value.ival = ConstValue(FT_BOOL,
		                        EvaluateBool(*node.children[0]) ? 0 : 1);
		value.type = MakeFundamentalType(FT_BOOL);
		return value;
	}
	case OP_PLUS:
	case OP_MINUS:
	case OP_COMPL:
	{
		EvalValue operand = EvalRead(*node.children[0]);
		if (operand.kind == EvalValue::EV_FLOAT)
		{
			EvalValue value = Convert(operand, ValueTypeOf(node));
			if (node.op == OP_MINUS)
				value.fval = -value.fval;
			else if (node.op == OP_COMPL)
				throw NotConstant("complement of a floating value");
			return value;
		}
		if (operand.kind != EvalValue::EV_INT)
			throw NotConstant("unary operand");
		EvalValue value;
		value.kind = EvalValue::EV_INT;
		value.ival = ConstIntUnary(node.op, operand.ival);
		value.type = ValueTypeOf(node);
		return value;
	}
	default:
		throw NotConstant("unary operator");
	}
}

// `p + n`, `p - n`, `p - q`, and the pointer comparisons.
EvalValue ConstEvalEngine::EvalPointerBinary(const SemNode& node,
                                             EvalValue lhs, EvalValue rhs)
{
	EvalValue value;
	if (node.op == OP_PLUS || node.op == OP_MINUS)
	{
		if (node.op == OP_MINUS && lhs.kind == EvalValue::EV_PTR &&
		    rhs.kind == EvalValue::EV_PTR)
		{
			if (lhs.ptr.object != rhs.ptr.object ||
			    lhs.ptr.sym_scope || rhs.ptr.sym_scope)
				throw NotConstant("difference of unrelated pointers");
			TypePtr element =
				RemoveTopCv(RemoveTopCv(lhs.type)->target);
			value.kind = EvalValue::EV_INT;
			value.ival = ConstValue(
				FT_LONG_INT,
				(unsigned long long)(((long long)lhs.ptr.offset -
				                      (long long)rhs.ptr.offset) /
				                     (long long)TypeSize(element)));
			value.type = ValueTypeOf(node);
			return value;
		}
		if (lhs.kind != EvalValue::EV_PTR)
			std::swap(lhs, rhs);
		if (lhs.kind != EvalValue::EV_PTR ||
		    rhs.kind != EvalValue::EV_INT)
			throw NotConstant("pointer arithmetic operands");
		TypePtr pointer_type = RemoveTopCv(ValueTypeOf(node));
		if (pointer_type->kind != TK_POINTER)
			throw NotConstant("pointer arithmetic form");
		long long count = (long long)rhs.ival.bits;
		if (node.op == OP_MINUS)
			count = -count;
		value = lhs;
		value.type = pointer_type;
		value.ptr.offset +=
			count * (long long)TypeSize(RemoveTopCv(pointer_type->target));
		return value;
	}
	// Comparisons: identity plus offset order within one object.
	bool same = lhs.ptr.object == rhs.ptr.object &&
		lhs.ptr.sym_scope == rhs.ptr.sym_scope &&
		lhs.ptr.sym_name == rhs.ptr.sym_name;
	bool equal = same && lhs.ptr.offset == rhs.ptr.offset;
	bool truth;
	switch (node.op)
	{
	case OP_EQ: truth = equal; break;
	case OP_NE: truth = !equal; break;
	case OP_LT: case OP_GT: case OP_LE: case OP_GE:
	{
		if (!same)
			throw NotConstant("order of unrelated pointers");
		unsigned long long a = lhs.ptr.offset;
		unsigned long long b = rhs.ptr.offset;
		truth = node.op == OP_LT ? a < b : node.op == OP_GT ? a > b
			: node.op == OP_LE ? a <= b : a >= b;
		break;
	}
	default:
		throw NotConstant("pointer operator");
	}
	EvalValue result;
	result.kind = EvalValue::EV_INT;
	result.ival = ConstValue(FT_BOOL, truth ? 1 : 0);
	result.type = MakeFundamentalType(FT_BOOL);
	return result;
}

EvalValue ConstEvalEngine::EvalBinary(const SemNode& node)
{
	if (!node.has_op)
		throw NotConstant("binary form");
	if (node.op == OP_LAND || node.op == OP_LOR)
	{
		bool lhs = EvaluateBool(*node.children[0]);
		bool result = (node.op == OP_LAND) == lhs
			? EvaluateBool(*node.children[1])
			: lhs;
		EvalValue value;
		value.kind = EvalValue::EV_INT;
		value.ival = ConstValue(FT_BOOL, result ? 1 : 0);
		value.type = MakeFundamentalType(FT_BOOL);
		return value;
	}
	if (node.op == OP_COMMA)
	{
		EvalEffect(*node.children[0]);
		return EvalRead(*node.children[1]);
	}
	EvalValue lhs = EvalRead(*node.children[0]);
	EvalValue rhs = EvalRead(*node.children[1]);
	if (lhs.kind == EvalValue::EV_PTR || rhs.kind == EvalValue::EV_PTR)
		return EvalPointerBinary(node, lhs, rhs);
	if (lhs.kind == EvalValue::EV_FLOAT ||
	    rhs.kind == EvalValue::EV_FLOAT)
	{
		long double a = lhs.kind == EvalValue::EV_FLOAT
			? lhs.fval
			: IsSignedIntegralFundamental(lhs.ival.type)
				? (long double)(long long)lhs.ival.bits
				: (long double)lhs.ival.bits;
		long double b = rhs.kind == EvalValue::EV_FLOAT
			? rhs.fval
			: IsSignedIntegralFundamental(rhs.ival.type)
				? (long double)(long long)rhs.ival.bits
				: (long double)rhs.ival.bits;
		EvalValue value;
		switch (node.op)
		{
		case OP_PLUS: value.fval = a + b; break;
		case OP_MINUS: value.fval = a - b; break;
		case OP_STAR: value.fval = a * b; break;
		case OP_DIV:
			if (b == 0)
				throw NotConstant("floating division by zero");
			value.fval = a / b;
			break;
		case OP_EQ: case OP_NE: case OP_LT: case OP_GT:
		case OP_LE: case OP_GE:
		{
			bool truth = node.op == OP_EQ ? a == b
				: node.op == OP_NE ? a != b : node.op == OP_LT ? a < b
				: node.op == OP_GT ? a > b : node.op == OP_LE ? a <= b
				: a >= b;
			value.kind = EvalValue::EV_INT;
			value.ival = ConstValue(FT_BOOL, truth ? 1 : 0);
			value.type = MakeFundamentalType(FT_BOOL);
			return value;
		}
		default:
			throw NotConstant("floating operator");
		}
		value.kind = EvalValue::EV_FLOAT;
		value.type = RemoveTopCv(ValueTypeOf(node));
		return Convert(value, value.type);
	}
	EvalValue value;
	value.kind = EvalValue::EV_INT;
	value.ival = ConstIntBinary(node.op, lhs.ival, rhs.ival);
	value.type = RemoveTopCv(ValueTypeOf(node));
	return value;
}

EvalValue ConstEvalEngine::EvalAssignment(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	if (node.member_ref)
	{
		// Reference binding (constructor member initialization): the
		// store binds the reference slot itself.
		ConstPointer slot = EvalLValue(*lhs.children[0]);
		slot.offset += lhs.member_offset;
		EvalValue bound;
		bound.kind = EvalValue::EV_PTR;
		bound.ptr = EvalLValue(*node.children[1]);
		WriteValue(slot, MakePointerType(ValueTypeOf(lhs), false, false),
		           bound);
		return bound;
	}
	ConstPointer target = EvalLValue(lhs);
	TypePtr type = RemoveTopCv(ValueTypeOf(lhs));
	EvalValue value = EvalRead(*node.children[1]);
	if (node.op != OP_ASS)
	{
		EvalValue current = ReadThrough(target, type);
		ETokenType base;
		switch (node.op)
		{
		case OP_PLUSASS: base = OP_PLUS; break;
		case OP_MINUSASS: base = OP_MINUS; break;
		case OP_STARASS: base = OP_STAR; break;
		case OP_DIVASS: base = OP_DIV; break;
		case OP_MODASS: base = OP_MOD; break;
		case OP_XORASS: base = OP_XOR; break;
		case OP_BANDASS: base = OP_AMP; break;
		case OP_BORASS: base = OP_BOR; break;
		case OP_LSHIFTASS: base = OP_LSHIFT; break;
		case OP_RSHIFTASS: base = OP_RSHIFT; break;
		default:
			throw NotConstant("assignment operator");
		}
		if (current.kind == EvalValue::EV_PTR)
		{
			SemNode proxy(SN_BINARY_EXPRESSION);
			proxy.has_op = true;
			proxy.op = base;
			proxy.type = ValueTypeOf(lhs);
			value = EvalPointerBinary(proxy, current, value);
		}
		else if (current.kind == EvalValue::EV_FLOAT ||
		         value.kind == EvalValue::EV_FLOAT)
		{
			EvalValue a = Convert(current, MakeFundamentalType(
				FT_LONG_DOUBLE));
			EvalValue b = Convert(value, MakeFundamentalType(
				FT_LONG_DOUBLE));
			EvalValue result;
			result.kind = EvalValue::EV_FLOAT;
			switch (base)
			{
			case OP_PLUS: result.fval = a.fval + b.fval; break;
			case OP_MINUS: result.fval = a.fval - b.fval; break;
			case OP_STAR: result.fval = a.fval * b.fval; break;
			case OP_DIV:
				if (b.fval == 0)
					throw NotConstant("floating division by zero");
				result.fval = a.fval / b.fval;
				break;
			default:
				throw NotConstant("floating compound assignment");
			}
			value = result;
		}
		else
		{
			EvalValue result;
			result.kind = EvalValue::EV_INT;
			result.ival = ConstIntBinary(base, current.ival, value.ival);
			value = result;
		}
	}
	value = Convert(value, type);
	WriteValue(target, type, value);
	return value;
}

EvalValue ConstEvalEngine::EvalIncDec(const SemNode& node, bool prefix)
{
	ConstPointer target = EvalLValue(*node.children[0]);
	TypePtr type = RemoveTopCv(ValueTypeOf(*node.children[0]));
	EvalValue current = ReadThrough(target, type);
	EvalValue updated = current;
	long long delta = node.op == OP_INC ? 1 : -1;
	if (current.kind == EvalValue::EV_PTR)
	{
		if (type->kind != TK_POINTER)
			throw NotConstant("increment form");
		updated.ptr.offset +=
			delta * (long long)TypeSize(RemoveTopCv(type->target));
	}
	else if (current.kind == EvalValue::EV_FLOAT)
		updated.fval += delta;
	else
	{
		updated.ival = ConvertConstValue(
			ConstIntBinary(node.op == OP_INC ? OP_PLUS : OP_MINUS,
			               current.ival,
			               ConstValue(FT_INT, 1)),
			current.ival.type);
	}
	WriteValue(target, type, updated);
	return prefix ? updated : current;
}

EvalValue ConstEvalEngine::EvalConditional(const SemNode& node)
{
	bool truth = EvaluateBool(*node.children[0]);
	return EvalRead(*node.children[truth ? 1 : 2]);
}

EvalValue ConstEvalEngine::EvalCast(const SemNode& node)
{
	if (IsLValueNode(node))
		return ReadThrough(EvalLValue(*node.children[0]),
		                   ValueTypeOf(node));
	return Convert(EvalRead(*node.children[0]), ValueTypeOf(node));
}

EvalValue ConstEvalEngine::EvalMember(const SemNode& node)
{
	TypePtr type = ValueTypeOf(node);
	if (RemoveTopCv(type)->kind == TK_ARRAY)
	{
		EvalValue value;
		value.kind = EvalValue::EV_PTR;
		value.ptr = MemberAddress(node);
		value.type = type;
		return value;
	}
	return ReadThrough(MemberAddress(node), type);
}

EvalValue ConstEvalEngine::EvalSubscript(const SemNode& node)
{
	return ReadThrough(SubscriptAddress(node), ValueTypeOf(node));
}

EvalValue ConstEvalEngine::EvalCallValue(const SemNode& node)
{
	if (IsClassValued(node) && node.category == VC_PRVALUE)
		throw NotConstant("class-valued call in a scalar context");
	EvalValue value = EvalCall(node, 0);
	if (IsLValueNode(node))
	{
		if (value.kind != EvalValue::EV_PTR || value.ptr.IsNull())
			throw NotConstant("reference call result");
		return ReadThrough(value.ptr, ValueTypeOf(node));
	}
	return value;
}

void ConstEvalEngine::EvalEffect(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_ASSIGNMENT_EXPRESSION:
		EvalAssignment(node);
		return;
	case SN_UNARY_EXPRESSION:
		if (node.has_op && (node.op == OP_INC || node.op == OP_DEC))
		{
			EvalIncDec(node, true);
			return;
		}
		break;
	case SN_POSTFIX_EXPRESSION:
		if (node.has_op && (node.op == OP_INC || node.op == OP_DEC))
		{
			EvalIncDec(node, false);
			return;
		}
		break;
	case SN_CALL_EXPRESSION:
		EvalCall(node, 0);
		return;
	case SN_BINARY_EXPRESSION:
		if (node.has_op && node.op == OP_COMMA)
		{
			EvalEffect(*node.children[0]);
			EvalEffect(*node.children[1]);
			return;
		}
		break;
	default:
		break;
	}
	EvalRead(node);
}

EvalValue ConstEvalEngine::EvalRead(const SemNode& node)
{
	Step();
	switch (node.kind)
	{
	case SN_LITERAL:
		return EvalLiteral(node);
	case SN_ID_EXPRESSION:
		return EvalId(node);
	case SN_UNARY_EXPRESSION:
		return EvalUnary(node);
	case SN_POSTFIX_EXPRESSION:
		if (node.has_op && (node.op == OP_INC || node.op == OP_DEC))
			return EvalIncDec(node, false);
		throw NotConstant("postfix operator");
	case SN_BINARY_EXPRESSION:
		return EvalBinary(node);
	case SN_ASSIGNMENT_EXPRESSION:
		return EvalAssignment(node);
	case SN_CONDITIONAL_EXPRESSION:
		return EvalConditional(node);
	case SN_SUBSCRIPT_EXPRESSION:
		return EvalSubscript(node);
	case SN_MEMBER_EXPRESSION:
		return EvalMember(node);
	case SN_CAST_EXPRESSION:
	case SN_SIZEOF_EXPRESSION:
		if (node.has_value)
		{
			EvalValue value;
			value.kind = EvalValue::EV_INT;
			value.ival = node.value;
			value.type = RemoveTopCv(ValueTypeOf(node));
			return value;
		}
		if (node.kind == SN_CAST_EXPRESSION)
			return EvalCast(node);
		throw NotConstant("sizeof form");
	case SN_CALL_EXPRESSION:
		return EvalCallValue(node);
	default:
		if (node.has_value)
		{
			EvalValue value;
			value.kind = EvalValue::EV_INT;
			value.ival = node.value;
			value.type = RemoveTopCv(ValueTypeOf(node));
			return value;
		}
		throw NotConstant("expression form");
	}
}
