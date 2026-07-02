#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"
#include "sema/const_expr.h"
#include "sema/sem_convert.h"

using std::runtime_error;
using std::to_string;

// Expression lowering: value, address, branch, and effect contexts
// over the resolved PA12 tree. The statement half and the shared
// block/slot/temp state live in lower_function.cpp.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

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

bool IsPointerish(const TypePtr& type)
{
	return type->kind == TK_POINTER || type->kind == TK_ARRAY ||
		type->kind == TK_FUNCTION || IsNullPtrType(type);
}

// The binary instruction operator of a token over `type`.
string BinaryOpName(ETokenType op, const TypePtr& type)
{
	bool unsigned_ops = LowerUnsignedOps(type);
	switch (op)
	{
	case OP_PLUS: case OP_PLUSASS: case OP_INC: return "add";
	case OP_MINUS: case OP_MINUSASS: case OP_DEC: return "sub";
	case OP_STAR: case OP_STARASS: return "mul";
	case OP_DIV: case OP_DIVASS: return unsigned_ops ? "udiv" : "div";
	case OP_MOD: case OP_MODASS: return unsigned_ops ? "umod" : "mod";
	case OP_AMP: case OP_BANDASS: return "and";
	case OP_BOR: case OP_BORASS: return "or";
	case OP_XOR: case OP_XORASS: return "xor";
	case OP_LSHIFT: case OP_LSHIFTASS: return "shl";
	case OP_RSHIFT: case OP_RSHIFTASS:
		return unsigned_ops ? "ushr" : "shr";
	default:
		throw OutsideBoundary("binary operator");
	}
}

string ComparePredicate(ETokenType op, const TypePtr& type)
{
	bool unsigned_ops = LowerUnsignedOps(type) && !LowerFloatType(type);
	switch (op)
	{
	case OP_EQ: return "eq";
	case OP_NE: return "ne";
	case OP_LT: return unsigned_ops ? "ult" : "lt";
	case OP_GT: return unsigned_ops ? "ugt" : "gt";
	case OP_LE: return unsigned_ops ? "ule" : "le";
	case OP_GE: return unsigned_ops ? "uge" : "ge";
	default:
		throw OutsideBoundary("comparison operator");
	}
}

bool IsComparisonOp(ETokenType op)
{
	return op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_GT ||
		op == OP_LE || op == OP_GE;
}

bool IsCompoundShift(ETokenType op)
{
	return op == OP_LSHIFTASS || op == OP_RSHIFTASS;
}

TypePtr BoolType()
{
	return MakeFundamentalType(FT_BOOL);
}

}  // namespace

const ScopeBinding* FunctionLowerer::EntityBinding(
	const SemNode& node) const
{
	if (!node.entity_scope)
		return 0;
	return FindOwnBinding(*node.entity_scope, node.entity_name);
}

string FunctionLowerer::MaterializeNull()
{
	string temp = NewTemp();
	Emit(temp + " = copy ptr nullptr");
	return temp;
}

// The canonical truth value branched on: floats compare against zero,
// everything else branches on the value directly.
LowerValue FunctionLowerer::MaterializeTruth(const LowerValue& value)
{
	if (LowerFloatType(value.type))
	{
		LowerValue truth;
		truth.text = NewTemp();
		truth.type = BoolType();
		Emit(truth.text + " = cmp ne " + LowerValueType(value.type) +
		     " " + value.text + ", " + LowerFloatZero(value.type));
		return truth;
	}
	if (value.imm_null && value.text.empty())
	{
		LowerValue made = value;
		made.text = MaterializeNull();
		made.imm_null = false;
		return made;
	}
	return value;
}

void FunctionLowerer::BranchOnValue(const SemNode& node,
                                    const string& true_label,
                                    const string& false_label)
{
	// Reference parity: a branch on a namespace-scope
	// pointer-to-function object spells the object's address - but
	// only when the program proves the stored value non-null and
	// unaliased (the address is truth-equivalent then); otherwise the
	// value loads like any other condition.
	if (node.kind == SN_ID_EXPRESSION && node.entity_scope &&
	    node.entity_scope->kind == SCOPE_NAMESPACE &&
	    node.type && RemoveTopCv(node.type)->kind == TK_POINTER &&
	    RemoveTopCv(node.type)->target->kind == TK_FUNCTION &&
	    program_.BranchSpellsFnPointerAddress(node.entity_scope,
	                                          node.entity_name))
	{
		string address = NewTemp();
		Emit(address + " = addr " +
		     program_.GlobalRef(node.entity_scope, node.entity_name));
		ReferenceLabel(true_label);
		ReferenceLabel(false_label);
		Terminate("branch " + address + ", ^" + true_label + ", ^" +
		          false_label);
		return;
	}
	LowerValue truth = MaterializeTruth(LowerValueExpr(node));
	ReferenceLabel(true_label);
	ReferenceLabel(false_label);
	Terminate("branch " + truth.text + ", ^" + true_label + ", ^" +
	          false_label);
}

// Statement-condition lowering: built-in && / || branch through their
// operand blocks directly (the README-pinned direct short-circuit
// shape); every other condition branches on its value.
void FunctionLowerer::LowerCondition(const SemNode& node,
                                     const string& true_label,
                                     const string& false_label)
{
	if (node.kind == SN_BINARY_EXPRESSION && node.op == OP_LAND)
	{
		string rhs_label = NewLabel("land_rhs");
		LowerCondition(*node.children[0], rhs_label, false_label);
		OpenBlock(rhs_label);
		LowerCondition(*node.children[1], true_label, false_label);
		return;
	}
	if (node.kind == SN_BINARY_EXPRESSION && node.op == OP_LOR)
	{
		string rhs_label = NewLabel("lor_rhs");
		LowerCondition(*node.children[0], true_label, rhs_label);
		OpenBlock(rhs_label);
		LowerCondition(*node.children[1], true_label, false_label);
		return;
	}
	BranchOnValue(node, true_label, false_label);
}

// --- values ---------------------------------------------------------------

LowerValue FunctionLowerer::LowerLiteralValue(const SemNode& node)
{
	LowerValue value;
	if (node.is_string_literal)
	{
		// 4.2: the literal object's address; no decay instruction is
		// spelled for string-literal views.
		string global = program_.StringLiteralRef(node);
		value.text = NewTemp();
		Emit(value.text + " = addr " + global);
		value.type = MakePointerType(node.type->target, false, false);
		return value;
	}
	value.type = NodeType(node);
	if (node.null_pointer && value.type->kind == TK_POINTER)
	{
		// A null pointer constant the semantics retyped keeps the
		// immediate 0; a value-initialized pointer prvalue spells the
		// immediate nullptr.
		value.imm_null = true;
		value.text = node.has_value ? "0" : "nullptr";
		return value;
	}
	if (node.null_pointer && IsNullPtrType(value.type))
	{
		value.imm_null = true;
		// A retyped integer zero keeps the immediate spelling; the
		// nullptr keyword materializes.
		if (node.has_value)
			value.text = "0";
		return value;
	}
	if (node.has_value)
	{
		value.imm_int = true;
		value.value = node.value;
		value.text = RenderConstValue(node.value);
		return value;
	}
	if (LowerFloatType(value.type))
	{
		value.imm_float = true;
		value.text = node.token;
		return value;
	}
	throw OutsideBoundary("literal form");
}

LowerValue FunctionLowerer::LowerIdValue(const SemNode& node)
{
	const ScopeBinding* binding = EntityBinding(node);
	// PA18: a function-template specialization has no scope binding;
	// its identity rides on the node.
	if (!binding && node.fn_spec)
	{
		LowerValue value;
		value.type = NodeType(node);
		string name = program_.FunctionRef(node.entity_scope,
		                                   node.entity_name, node.type,
		                                   node.fn_spec);
		value.text = NewTemp();
		Emit(value.text + " = addr " + name);
		return value;
	}
	if (!binding)
		throw runtime_error("unresolved id-expression " + node.name);
	LowerValue value;
	value.type = NodeType(node);
	if (binding->kind == SB_FUNCTION)
	{
		string name = program_.FunctionRef(node.entity_scope,
		                                   node.entity_name, node.type);
		value.text = NewTemp();
		Emit(value.text + " = addr " + name);
		return value;
	}
	bool global = node.entity_scope->kind == SCOPE_NAMESPACE ||
		node.entity_scope->kind == SCOPE_CLASS;
	string storage = global
		? program_.GlobalRef(node.entity_scope, node.entity_name)
		: "$" + SlotRef(node.entity_scope, node.entity_name);
	if (IsReferenceType(binding->type))
	{
		string address = NewTemp();
		Emit(address + " = load ptr " + storage);
		if (value.type->kind == TK_FUNCTION ||
		    value.type->kind == TK_ARRAY)
		{
			value.text = address;
			return value;
		}
		value.text = NewTemp();
		Emit(value.text + " = load " + LowerValueType(value.type) +
		     " " + address);
		return value;
	}
	if (binding->type->kind == TK_ARRAY)
	{
		value.text = NewTemp();
		value.type = RemoveTopCv(binding->type);
		Emit(value.text + " = addr " + storage);
		return value;
	}
	value.text = NewTemp();
	Emit(value.text + " = load " + LowerValueType(value.type) + " " +
	     storage);
	return value;
}

LowerValue FunctionLowerer::LowerValueExpr(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_LITERAL:
		return LowerLiteralValue(node);
	case SN_ID_EXPRESSION:
		return LowerIdValue(node);
	case SN_CALL_EXPRESSION:
	{
		LowerValue result = LowerCall(node);
		if (!IsReferenceType(node.type))
			return result;
		LowerValue loaded;
		loaded.type = NodeType(node);
		loaded.text = NewTemp();
		Emit(loaded.text + " = load " + LowerValueType(loaded.type) +
		     " " + result.text);
		return loaded;
	}
	case SN_UNARY_EXPRESSION:
		return LowerUnary(node);
	case SN_POSTFIX_EXPRESSION:
		return LowerIncDec(node, false);
	case SN_BINARY_EXPRESSION:
		if (node.op == OP_LAND || node.op == OP_LOR)
			return LowerLogicalValue(node);
		if (IsComparisonOp(node.op))
			return LowerComparison(node);
		if (node.op == OP_COMMA)
		{
			LowerEffect(*node.children[0]);
			return LowerValueExpr(*node.children[1]);
		}
		return LowerBinary(node);
	case SN_ASSIGNMENT_EXPRESSION:
		return LowerAssignment(node);
	case SN_CONDITIONAL_EXPRESSION:
		return LowerConditionalValue(node);
	case SN_SUBSCRIPT_EXPRESSION:
	{
		string address = LowerAddressExpr(node);
		LowerValue value;
		value.type = NodeType(node);
		value.text = NewTemp();
		Emit(value.text + " = load " + LowerValueType(value.type) +
		     " " + address);
		return value;
	}
	case SN_MEMBER_EXPRESSION:
		return LowerMemberValue(node);
	case SN_NEW_ARRAY:
		return LowerNewArray(node);
	case SN_NEW_INIT:
		return LowerNewInit(node);
	case SN_CONSTRUCTOR_ACTION:
	{
		if (RemoveTopCv(node.type)->kind == TK_POINTER)
			return LowerNewConstruction(node);
		// A constructed temporary used as a value: its address temp.
		LowerValue value;
		value.type = RemoveTopCv(node.type);
		value.text = MaterializeTemporary(node, "tmpobj", true);
		return value;
	}
	case SN_CAST_EXPRESSION:
	{
		if (IsVoidType(node.type))
		{
			LowerEffect(*node.children[0]);
			LowerValue value;
			value.type = NodeType(node);
			return value;
		}
		// A cast of an array to a pointer spells the decay.
		if (NodeType(node)->kind == TK_POINTER &&
		    NodeType(*node.children[0])->kind == TK_ARRAY)
		{
			LowerValue value;
			value.type = NodeType(node);
			value.text = LowerPointerOperand(*node.children[0]);
			return value;
		}
		LowerValue operand = LowerValueExpr(*node.children[0]);
		return ConvertValue(operand, NodeType(node), LCC_CAST);
	}
	case SN_SIZEOF_EXPRESSION:
	{
		// sizeof materializes its constant explicitly.
		LowerValue value;
		value.type = NodeType(node);
		value.text = NewTemp();
		Emit(value.text + " = const i64 " +
		     RenderConstValue(node.value));
		return value;
	}
	default:
		throw OutsideBoundary("expression form");
	}
}

LowerValue FunctionLowerer::LowerUnary(const SemNode& node)
{
	const SemNode& operand = *node.children[0];
	LowerValue value;
	value.type = NodeType(node);
	switch (node.op)
	{
	case OP_STAR:
	{
		string pointer = LowerPointerOperand(operand);
		value.text = NewTemp();
		Emit(value.text + " = load " + LowerValueType(value.type) +
		     " " + pointer);
		return value;
	}
	case OP_AMP:
		value.text = LowerAddressExpr(operand);
		return value;
	case OP_INC:
	case OP_DEC:
		return LowerIncDec(node, true);
	case OP_LNOT:
	{
		LowerValue inner = LowerValueExpr(operand);
		value.text = NewTemp();
		value.type = BoolType();
		if (LowerFloatType(inner.type))
			Emit(value.text + " = cmp eq " +
			     LowerValueType(inner.type) + " " + inner.text +
			     ", " + LowerFloatZero(inner.type));
		else
			Emit(value.text + " = cmp eq i64 " + inner.text + ", 0");
		return value;
	}
	case OP_PLUS:
	{
		if (value.type->kind == TK_POINTER &&
		    NodeType(operand)->kind != TK_POINTER)
		{
			value.text = LowerPointerOperand(operand);
			return value;
		}
		LowerValue inner = LowerValueExpr(operand);
		return ConvertValue(inner, value.type, LCC_OPERAND);
	}
	case OP_MINUS:
	case OP_COMPL:
	{
		LowerValue inner = ConvertValue(LowerValueExpr(operand),
		                                value.type, LCC_OPERAND);
		value.text = NewTemp();
		Emit(value.text + " = unary " +
		     (node.op == OP_MINUS ? "neg " : "bitnot ") +
		     LowerValueType(value.type) + " " + inner.text);
		return value;
	}
	default:
		throw OutsideBoundary("unary operator");
	}
}

string FunctionLowerer::PointerStep(const string& base,
                                    const LowerValue& count,
                                    const TypePtr& element, bool negative)
{
	unsigned long long size = TypeSize(RemoveTopCv(element));
	string counted = count.text;
	// An 8-byte unsigned count re-reads as the signed index type
	// (both spell i64; the reference pins the conversion copy).
	if (!count.imm_int && count.type)
	{
		TypePtr bare = RemoveTopCv(StripRef(count.type));
		if (bare->kind == TK_FUNDAMENTAL &&
		    IsIntegralFundamental(bare->fundamental) &&
		    !IsSignedIntegralFundamental(bare->fundamental) &&
		    TypeSize(bare) == 8)
		{
			counted = NewTemp();
			Emit(counted + " = copy i64 " + count.text);
		}
	}
	string offset = counted;
	if (size != 1)
	{
		offset = NewTemp();
		Emit(offset + " = binary mul i64 " + counted + ", " +
		     to_string(size));
	}
	if (negative)
	{
		string negated = NewTemp();
		Emit(negated + " = binary sub i64 0, " + offset);
		offset = negated;
	}
	string result = NewTemp();
	Emit(result + " = index i8 " + base + ", " + offset);
	return result;
}

LowerValue FunctionLowerer::LowerBinary(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	const SemNode& rhs = *node.children[1];
	LowerValue value;
	value.type = NodeType(node);
	if (value.type->kind == TK_POINTER)
	{
		// 5.7: pointer +/- integer in source order, byte-scaled.
		bool left_pointer = IsPointerish(NodeType(lhs));
		string base;
		LowerValue count;
		if (left_pointer)
		{
			base = LowerPointerOperand(lhs);
			count = LowerValueExpr(rhs);
		}
		else
		{
			count = LowerValueExpr(lhs);
			base = LowerPointerOperand(rhs);
		}
		value.text = PointerStep(base, count, value.type->target,
		                         node.op == OP_MINUS);
		return value;
	}
	if (IsPointerish(NodeType(lhs)) && IsPointerish(NodeType(rhs)) &&
	    node.op == OP_MINUS)
	{
		// 5.7p6: pointer difference scales back by the element size.
		string a = LowerPointerOperand(lhs);
		string b = LowerPointerOperand(rhs);
		string diff = NewTemp();
		Emit(diff + " = binary sub ptr " + a + ", " + b);
		TypePtr element = NodeType(lhs)->target;
		value.text = NewTemp();
		Emit(value.text + " = binary div i64 " + diff + ", " +
		     to_string(TypeSize(RemoveTopCv(element))));
		return value;
	}
	LowerValue a = LowerValueExpr(lhs);
	LowerValue b = LowerValueExpr(rhs);
	a = ConvertValue(a, value.type, LCC_OPERAND);
	b = ConvertValue(b, value.type, LCC_OPERAND);
	value.text = NewTemp();
	Emit(value.text + " = binary " + BinaryOpName(node.op, value.type) +
	     " " + LowerValueType(value.type) + " " + a.text + ", " +
	     b.text);
	return value;
}

// One side of a pointer comparison: null constants materialize,
// arrays and functions decay.
string FunctionLowerer::LowerPointerCmpOperand(const SemNode& node)
{
	TypePtr type = NodeType(node);
	if (node.null_pointer &&
	    (type->kind == TK_POINTER || IsIntegralType(type)))
		return "0";
	if (node.null_pointer && IsNullPtrType(type))
		return MaterializeNull();
	if (IsNullPtrType(type))
	{
		LowerValue value = LowerValueExpr(node);
		return value.imm_null ? MaterializeNull() : value.text;
	}
	return LowerPointerOperand(node);
}

LowerValue FunctionLowerer::LowerComparison(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	const SemNode& rhs = *node.children[1];
	LowerValue value;
	value.type = BoolType();
	if (IsPointerish(NodeType(lhs)) || IsPointerish(NodeType(rhs)))
	{
		string a = LowerPointerCmpOperand(lhs);
		string b = LowerPointerCmpOperand(rhs);
		value.text = NewTemp();
		Emit(value.text + " = cmp " +
		     ComparePredicate(node.op,
		                      MakePointerType(MakeFundamentalType(
		                          FT_VOID), false, false)) +
		     " ptr " + a + ", " + b);
		return value;
	}
	TypePtr lt = NodeType(lhs);
	TypePtr common;
	if (lt->kind == TK_ENUM && lt->named->is_scoped)
		common = lt;
	else
		common = UsualArithmeticConversions(lt, NodeType(rhs));
	LowerValue a = LowerValueExpr(lhs);
	LowerValue b = LowerValueExpr(rhs);
	a = ConvertValue(a, common, LCC_OPERAND);
	b = ConvertValue(b, common, LCC_OPERAND);
	value.text = NewTemp();
	Emit(value.text + " = cmp " + ComparePredicate(node.op, common) +
	     " " + LowerValueType(common) + " " + a.text + ", " + b.text);
	return value;
}

LowerValue FunctionLowerer::LowerLogicalValue(const SemNode& node)
{
	bool is_and = node.op == OP_LAND;
	const char* family = is_and ? "land" : "lor";
	string slot = AddMatSlot(family, "i64");
	string rhs_label = NewLabel(string(family) + "_rhs");
	string short_label = NewLabel(string(family) + "_short");
	string end_label = NewLabel(string(family) + "_end");
	if (is_and)
		BranchOnValue(*node.children[0], rhs_label, short_label);
	else
		BranchOnValue(*node.children[0], short_label, rhs_label);
	OpenBlock(rhs_label);
	size_t rhs_mark = temp_cleanups_.size();
	LowerValue operand = LowerValueExpr(*node.children[1]);
	string truth = NewTemp();
	if (LowerFloatType(operand.type))
		Emit(truth + " = cmp ne " + LowerValueType(operand.type) +
		     " " + operand.text + ", " + LowerFloatZero(operand.type));
	else
		Emit(truth + " = cmp ne i64 " + operand.text + ", 0");
	Emit("store i64 " + truth + ", $" + slot);
	// 12.2: temporaries of the conditionally evaluated operand die
	// inside its arm.
	if (temp_cleanups_.size() > rhs_mark && !blocks_.back().terminated)
		EmitTempCleanups(rhs_mark);
	if (eh_open_)
		CloseEhRegion();
	temp_cleanups_.resize(rhs_mark);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(short_label);
	Emit(string("store i64 ") + (is_and ? "0" : "1") + ", $" + slot);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(end_label);
	LowerValue value;
	value.type = BoolType();
	value.text = NewTemp();
	Emit(value.text + " = load i64 $" + slot);
	return value;
}

LowerValue FunctionLowerer::LowerConditionalValue(const SemNode& node)
{
	string then_label = NewLabel("cond_then");
	string else_label = NewLabel("cond_else");
	string end_label = NewLabel("cond_end");
	if (IsVoidType(node.type))
	{
		BranchOnValue(*node.children[0], then_label, else_label);
		OpenBlock(then_label);
		LowerEffect(*node.children[1]);
		CloseInto(else_label);
		LowerEffect(*node.children[2]);
		CloseInto(end_label);
		LowerValue value;
		value.type = NodeType(node);
		return value;
	}
	TypePtr type = NodeType(node);
	string slot = AddMatSlot("cond", LowerValueType(type));
	BranchOnValue(*node.children[0], then_label, else_label);
	OpenBlock(then_label);
	string first = LowerValueAs(*node.children[1], type, LCC_INIT);
	Emit("store " + LowerValueType(type) + " " + first + ", $" + slot);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(else_label);
	string second = LowerValueAs(*node.children[2], type, LCC_INIT);
	Emit("store " + LowerValueType(type) + " " + second + ", $" + slot);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(end_label);
	LowerValue value;
	value.type = type;
	value.text = NewTemp();
	Emit(value.text + " = load " + LowerValueType(type) + " $" + slot);
	return value;
}

string FunctionLowerer::LowerConditionalAddress(const SemNode& node)
{
	string slot = AddMatSlot("condaddr", "ptr");
	string then_label = NewLabel("condaddr_then");
	string else_label = NewLabel("condaddr_else");
	string end_label = NewLabel("condaddr_end");
	TypePtr result_type = NodeType(node);
	BranchOnValue(*node.children[0], then_label, else_label);
	for (int arm = 1; arm <= 2; arm++)
	{
		OpenBlock(arm == 1 ? then_label : else_label);
		const SemNode& operand = *node.children[arm];
		string address = LowerAddressExpr(operand);
		TypePtr source = NodeType(operand);
		// A derived arm adjusts to the common base result (5.16p3).
		if (source->kind == TK_CLASS && result_type->kind == TK_CLASS)
		{
			address = AdjustToBase(
				address,
				BaseClassDistance(source->named, result_type->named));
		}
		Emit("store ptr " + address + ", $" + slot);
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	OpenBlock(end_label);
	string result = NewTemp();
	Emit(result + " = load ptr $" + slot);
	return result;
}

// --- increments, assignments ----------------------------------------------

// The directly addressable storage of an id-expression lvalue
// ("$slot" or "@global"), or "" when an address must be computed.
string FunctionLowerer::DirectStorage(const SemNode& node)
{
	if (node.kind != SN_ID_EXPRESSION)
		return "";
	if (address_aliases_.count(std::make_pair(
			(const void*)node.entity_scope, node.entity_name)))
		return "";  // addressed through the alias, not a slot
	const ScopeBinding* binding = EntityBinding(node);
	if (!binding || binding->kind == SB_FUNCTION ||
	    IsReferenceType(binding->type) ||
	    binding->type->kind == TK_ARRAY)
		return "";
	if (node.entity_scope->kind == SCOPE_NAMESPACE ||
	    node.entity_scope->kind == SCOPE_CLASS)
		return program_.GlobalRef(node.entity_scope, node.entity_name);
	return "$" + SlotRef(node.entity_scope, node.entity_name);
}

LowerValue FunctionLowerer::LowerIncDec(const SemNode& node, bool prefix)
{
	const SemNode& operand = *node.children[0];
	TypePtr type = NodeType(node);
	string type_text = LowerValueType(type);
	string storage = DirectStorage(operand);
	if (storage.empty())
		storage = LowerAddressExpr(operand);
	string old_value = NewTemp();
	Emit(old_value + " = load " + type_text + " " + storage);
	string new_value;
	if (type->kind == TK_POINTER)
	{
		LowerValue one;
		one.text = "1";
		new_value = PointerStep(old_value, one, type->target,
		                        node.op == OP_DEC);
	}
	else
	{
		new_value = NewTemp();
		string one = LowerFloatType(type)
			? (type->fundamental == FT_FLOAT ? "1.0f"
			   : type->fundamental == FT_LONG_DOUBLE ? "1.0L" : "1.0")
			: string("1");
		Emit(new_value + " = binary " + BinaryOpName(node.op, type) +
		     " " + type_text + " " + old_value + ", " + one);
	}
	Emit("store " + type_text + " " + new_value + ", " + storage);
	LowerValue value;
	value.type = type;
	value.text = prefix ? new_value : old_value;
	return value;
}

LowerValue FunctionLowerer::LowerAssignment(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	const SemNode& rhs = *node.children[1];
	if (node.op == OP_ASS &&
	    (node.member_ref || lhs.kind == SN_MEMBER_EXPRESSION))
		return LowerMemberAssignment(node);
	TypePtr type = NodeType(lhs);
	if (node.op != OP_ASS)
	{
		LowerValue target;
		target.text = DirectStorage(lhs);
		if (target.text.empty())
			target.text = LowerAddressExpr(lhs);
		target.type = type;
		// A member lvalue recomputes its store address after the value
		// (the canonical reference order).
		return LowerCompoundAssignment(
			node, target,
			lhs.kind == SN_MEMBER_EXPRESSION ? &lhs : 0);
	}
	// 5.17 in the canonical reference order: the value computes before
	// the store address.
	string value = LowerValueAs(rhs, type, LCC_OPERAND);
	string storage = DirectStorage(lhs);
	if (storage.empty())
		storage = LowerAddressExpr(lhs);
	Emit("store " + LowerValueType(type) + " " + value + ", " +
	     storage);
	LowerValue result;
	result.type = type;
	result.text = value;
	return result;
}

LowerValue FunctionLowerer::LowerCompoundAssignment(
	const SemNode& node, const LowerValue& target,
	const SemNode* member_lhs)
{
	const SemNode& rhs = *node.children[1];
	TypePtr type = target.type;
	string type_text = LowerValueType(type);
	string old_value = NewTemp();
	Emit(old_value + " = load " + type_text + " " + target.text);
	LowerValue result;
	result.type = type;
	if (type->kind == TK_POINTER)
	{
		// 5.17p7 over 5.7: element-size scaled pointer adjustment.
		LowerValue count = LowerValueExpr(rhs);
		result.text = PointerStep(old_value, count, type->target,
		                          node.op == OP_MINUSASS);
		Emit("store ptr " + result.text + ", " +
		     (member_lhs ? MemberAddress(*member_lhs) : target.text));
		return result;
	}
	TypePtr common = IsCompoundShift(node.op)
		? PromoteForArithmetic(type)
		: UsualArithmeticConversions(type, NodeType(rhs));
	LowerValue old_lv;
	old_lv.text = old_value;
	old_lv.type = type;
	LowerValue count = LowerValueExpr(rhs);
	old_lv = ConvertValue(old_lv, common, LCC_OPERAND);
	count = ConvertValue(count, common, LCC_OPERAND);
	string computed = NewTemp();
	Emit(computed + " = binary " + BinaryOpName(node.op, common) + " " +
	     LowerValueType(common) + " " + old_lv.text + ", " +
	     count.text);
	LowerValue back;
	back.text = computed;
	back.type = common;
	back = ConvertValue(back, type, LCC_OPERAND);
	Emit("store " + type_text + " " + back.text + ", " +
	     (member_lhs ? MemberAddress(*member_lhs) : target.text));
	result.text = back.text;
	return result;
}

// --- addresses and pointers -------------------------------------------------

string FunctionLowerer::LowerAddressExpr(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_ID_EXPRESSION:
	{
		// A by_address parameter (or the return-slot-reused local)
		// addresses the caller-owned object directly.
		map<pair<const void*, string>, string>::const_iterator alias =
			address_aliases_.find(std::make_pair(
				(const void*)node.entity_scope, node.entity_name));
		if (alias != address_aliases_.end())
			return alias->second;
		const ScopeBinding* binding = EntityBinding(node);
		// PA18: a function-template specialization has no scope
		// binding; its identity rides on the node.
		if (!binding && node.fn_spec)
		{
			string name = program_.FunctionRef(
				node.entity_scope, node.entity_name, node.type,
				node.fn_spec);
			string temp = NewTemp();
			Emit(temp + " = addr " + name);
			return temp;
		}
		if (!binding)
			throw runtime_error("unresolved lvalue " + node.name);
		if (binding->kind == SB_FUNCTION)
		{
			string name = program_.FunctionRef(
				node.entity_scope, node.entity_name, node.type);
			string temp = NewTemp();
			Emit(temp + " = addr " + name);
			return temp;
		}
		bool global = node.entity_scope->kind == SCOPE_NAMESPACE ||
			node.entity_scope->kind == SCOPE_CLASS;
		string storage = global
			? program_.GlobalRef(node.entity_scope, node.entity_name)
			: "$" + SlotRef(node.entity_scope, node.entity_name);
		string temp = NewTemp();
		if (IsReferenceType(binding->type))
			Emit(temp + " = load ptr " + storage);
		else
			Emit(temp + " = addr " + storage);
		return temp;
	}
	case SN_SUBSCRIPT_EXPRESSION:
	{
		string base = LowerPointerOperand(*node.children[0]);
		LowerValue index = LowerValueExpr(*node.children[1]);
		TypePtr element = NodeType(node);
		if (element->kind == TK_CLASS)
			return ClassArrayElement(base, index, element);
		string temp = NewTemp();
		Emit(temp + " = index " + LowerValueType(element) +
		     " [projection=array_element] " + base + ", " +
		     index.text);
		return temp;
	}
	case SN_MEMBER_EXPRESSION:
		return MemberAddress(node);
	case SN_CONSTRUCTOR_ACTION:
		return MaterializeTemporary(node, "tmpobj", true);
	case SN_UNARY_EXPRESSION:
		if (node.op == OP_STAR)
			return LowerPointerOperand(*node.children[0]);
		if (node.op == OP_INC || node.op == OP_DEC)
		{
			LowerIncDec(node, true);
			return LowerAddressExpr(*node.children[0]);
		}
		throw OutsideBoundary("address form");
	case SN_CALL_EXPRESSION:
		if (!IsReferenceType(node.type))
		{
			if (NodeType(node)->kind == TK_CLASS)
				// A class prvalue call: the materialized result.
				return MaterializeClassResult(node, "tmpobj", "");
			throw OutsideBoundary("address of a call result");
		}
		return LowerCall(node).text;
	case SN_CONDITIONAL_EXPRESSION:
		return LowerConditionalAddress(node);
	case SN_BINARY_EXPRESSION:
		if (node.op == OP_COMMA)
		{
			LowerEffect(*node.children[0]);
			return LowerAddressExpr(*node.children[1]);
		}
		throw OutsideBoundary("address form");
	case SN_ASSIGNMENT_EXPRESSION:
		LowerAssignment(node);
		return LowerAddressExpr(*node.children[0]);
	case SN_LITERAL:
		if (node.is_string_literal)
			return LowerLiteralValue(node).text;
		throw OutsideBoundary("address form");
	default:
		throw OutsideBoundary("address form");
	}
}

string FunctionLowerer::LowerPointerOperand(const SemNode& node)
{
	TypePtr type = NodeType(node);
	if (type->kind == TK_ARRAY)
	{
		if (node.is_string_literal)
			return LowerLiteralValue(node).text;
		if (node.kind == SN_ID_EXPRESSION ||
		    node.kind == SN_MEMBER_EXPRESSION)
		{
			string address = LowerAddressExpr(node);
			string decayed = NewTemp();
			Emit(decayed + " = unary decay ptr " + address);
			return decayed;
		}
		// Synthesized array addresses (glvalue conditionals) are
		// already pointer-shaped.
		return LowerAddressExpr(node);
	}
	if (type->kind == TK_FUNCTION)
	{
		const ScopeBinding* binding = node.kind == SN_ID_EXPRESSION
			? EntityBinding(node) : 0;
		string address;
		if (binding && binding->kind == SB_FUNCTION)
			address = LowerAddressExpr(node);
		else
			address = LowerValueExpr(node).text;
		string decayed = NewTemp();
		Emit(decayed + " = unary decay ptr " + address);
		return decayed;
	}
	LowerValue value = LowerValueExpr(node);
	if (value.imm_null && value.text.empty())
		return MaterializeNull();
	return value.text;
}

// --- calls -------------------------------------------------------------------

string FunctionLowerer::LowerReferenceArgument(const SemNode& node,
                                               const TypePtr& referee)
{
	TypePtr bare = RemoveTopCv(referee);
	if (node.kind == SN_CONSTRUCTOR_ACTION && bare->kind == TK_CLASS)
	{
		// A class temporary binding a reference parameter: an exact
		// binding materializes as an argument object; a derived
		// temporary adjusts to the base referee (and keeps its own
		// dispatch region while live cleanups may unwind past it).
		TypePtr made = RemoveTopCv(node.type);
		bool exact = TypeEquals(made, bare);
		if (!exact && eh_armed_ && !eh_open_ && !in_cleanup_emission_ &&
		    program_.CalleeMayUnwind(*node.children[0]->children[0]))
			OpenEhRegion();
		string address =
			MaterializeTemporary(node, exact ? "arg" : "tmpobj",
			                     true);
		if (!exact && made->kind == TK_CLASS)
		{
			address = AdjustToBase(
				address,
				BaseClassDistance(made->named, bare->named));
		}
		return address;
	}
	TypePtr source = RemoveTopCv(StripRef(node.type));
	bool binds_directly = node.category != VC_PRVALUE &&
		(TypeEquals(source, bare) ||
		 (source->kind == TK_CLASS && bare->kind == TK_CLASS &&
		  BaseClassDistance(source->named, bare->named) >= 0));
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
			address = AdjustToBase(
				address,
				BaseClassDistance(source->named, bare->named));
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
		return AdjustToBase(
			address, BaseClassDistance(source->named, bare->named));
	}
	// 8.5.3p5: materialize a temporary with the converted value.
	string slot = AddMatSlot("refarg", LowerSlotType(bare));
	string value = LowerValueAs(node, bare, LCC_INIT);
	Emit("store " + LowerValueType(bare) + " " + value + ", $" + slot);
	string temp = NewTemp();
	Emit(temp + " = addr $" + slot);
	return temp;
}

string FunctionLowerer::LowerCallArgument(const SemNode& node,
                                          const TypePtr& param)
{
	if (!param)
	{
		// 5.2.2p7 default argument promotions for trailing variadic
		// arguments.
		TypePtr source = NodeType(node);
		TypePtr promoted = source;
		if (source->kind == TK_FUNDAMENTAL &&
		    source->fundamental == FT_FLOAT)
			promoted = MakeFundamentalType(FT_DOUBLE);
		else if (IsIntegralType(source) || IsUnscopedEnum(source))
			promoted = PromoteForArithmetic(source);
		else if (source->kind == TK_ARRAY ||
		         source->kind == TK_FUNCTION)
			return LowerPointerOperand(node);
		return LowerValueAs(node, promoted, LCC_INIT);
	}
	if (IsReferenceType(param))
		return LowerReferenceArgument(node, param->target);
	if (RemoveTopCv(param)->kind == TK_CLASS)
	{
		TypePtr bare = RemoveTopCv(param);
		if (!LowerClassDirect(bare))
			// The indirect boundary: the caller materializes the
			// argument object and passes its address.
			return MaterializeClassArg(node, bare);
		// The direct obj boundary: a fresh object slot passes by name.
		string slot = AddMatSlot("argobj", LowerSlotType(bare));
		string copy_address = NewTemp();
		Emit(copy_address + " = addr $" + slot);
		if (node.kind == SN_CONSTRUCTOR_ACTION && node.trivial_init)
			;  // default-constructed argument object: storage only
		else if (node.kind == SN_ID_EXPRESSION ||
		         node.kind == SN_MEMBER_EXPRESSION)
		{
			// The PA15 empty-class direct binding kept the slot
			// untouched; non-empty trivial sources copy their bytes.
			string source_address = LowerAddressExpr(node);
			if (bare->named->class_record &&
			    !bare->named->class_record->is_empty)
				Emit("copyobj " + LowerObjSpan(bare) + " " +
				     source_address + ", " + copy_address);
		}
		else
			LowerClassInit(node, copy_address);
		return "$" + slot;
	}
	return LowerValueAs(node, RemoveTopCv(param), LCC_INIT);
}

string FunctionLowerer::MaterializeClassArg(const SemNode& node,
                                            const TypePtr& bare)
{
	if (node.kind == SN_CONSTRUCTOR_ACTION)
		// The callee destroys its by-value parameter object;
		// no caller-side cleanup registers.
		return MaterializeTemporary(node, "arg", false);
	string slot = AddMatSlot("arg", LowerSlotType(bare));
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	LowerClassInit(node, address);
	return address;
}

LowerValue FunctionLowerer::LowerCall(const SemNode& node,
                                      const string& result_address)
{
	const SemNode& callee = *node.children[0];
	TypePtr fn_type;
	bool direct = callee.kind == SN_CALLEE;
	if (direct)
		fn_type = callee.type;
	else
	{
		TypePtr through = NodeType(callee);
		if (through->kind == TK_POINTER)
			fn_type = through->target;
		else if (through->kind == TK_FUNCTION)
			fn_type = through;
		else
			throw OutsideBoundary("call form");
	}
	// A call that can unwind while destructor-protected objects are
	// alive runs under an unwind-dispatch region. The result
	// preservation slot allocates at region entry.
	bool protected_call = !in_cleanup_emission_ &&
		(eh_armed_ || (!in_lifetime_action_ && HaveCleanups())) &&
		(!direct || program_.CalleeMayUnwind(callee));
	string preserve_slot;
	if (protected_call && !eh_open_)
		OpenEhRegion();
	if (protected_call && !IsVoidType(fn_type->target) &&
	    RemoveTopCv(fn_type->target)->kind != TK_CLASS)
		preserve_slot = AddMatSlot("call",
		                           LowerValueType(fn_type->target));
	string arguments;
	string object_text;  // the implicit object argument (dispatch)
	for (size_t i = 1; i < node.children.size(); i++)
	{
		TypePtr param = i - 1 < fn_type->parameters.size()
			? fn_type->parameters[i - 1] : TypePtr();
		string text = LowerCallArgument(*node.children[i], param);
		if (i == 1)
			object_text = text;
		arguments += (i > 1 ? ", " : "") + text;
	}
	// The callee value of an indirect call lowers after the arguments
	// (the oracle's evaluation order).
	bool dispatch = direct && callee.vtable_slot >= 0;
	string callee_text;
	if (dispatch)
	{
		// PA17 dynamic dispatch: the callee loads from the object's
		// vpointer at the resolved slot.
		string vpointer = NewTemp();
		Emit(vpointer + " = load ptr " + object_text);
		string slot_address = vpointer;
		if (callee.vtable_slot > 0)
		{
			slot_address = NewTemp();
			Emit(slot_address + " = index i8 " + vpointer + ", " +
			     to_string(8 * callee.vtable_slot));
		}
		callee_text = NewTemp();
		Emit(callee_text + " = load ptr " + slot_address);
	}
	else if (direct && (callee.is_method || callee.special != SF_NONE))
		callee_text = program_.MemberFunctionRef(callee);
	else if (direct)
		callee_text = program_.FunctionRef(callee.entity_scope,
		                                   callee.entity_name, fn_type);
	else if (NodeType(callee)->kind == TK_POINTER)
		callee_text = LowerValueExpr(callee).text;
	else
		callee_text = LowerPointerOperand(callee);
	string return_text;
	bool indirect_result = LowerAbiReturn(fn_type->target, return_text);
	if (indirect_result)
	{
		// The caller-owned result object's address leads the argument
		// list.
		if (result_address.empty())
			throw OutsideBoundary("indirect class result destination");
		arguments = result_address +
			(arguments.empty() ? "" : ", " + arguments);
	}
	string line;
	LowerValue result;
	result.type = StripRef(fn_type->target);
	if (return_text == "void")
	{
		line = "call void " + callee_text + "(" + arguments + ")";
		result.text = result_address;
	}
	else
	{
		result.text = NewTemp();
		line = result.text + " = call " + return_text + " " +
			callee_text + "(" + arguments + ")";
	}
	// Argument-temporary registration may have closed the region; the
	// call itself still runs protected.
	if (protected_call && !eh_open_ && !in_cleanup_emission_)
		OpenEhRegion();
	if (!direct || dispatch)
		line += IndirectCallSignature(fn_type);
	Emit(line);
	bool class_result =
		RemoveTopCv(fn_type->target)->kind == TK_CLASS &&
		!IsReferenceType(fn_type->target);
	if (protected_call && eh_open_ && !IsVoidType(fn_type->target) &&
	    !class_result && !preserve_slot.empty())
	{
		// A protected call's result survives the region in a slot (a
		// reference result survives as its pointer).
		Emit("store " + return_text + " " + result.text + ", $" +
		     preserve_slot);
		string reloaded = NewTemp();
		Emit(reloaded + " = load " + return_text + " $" + preserve_slot);
		result.text = reloaded;
	}
	return result;
}

// --- effects ---------------------------------------------------------------

void FunctionLowerer::LowerEffect(const SemNode& node)
{
	switch (node.kind)
	{
	case SN_ASSIGNMENT_EXPRESSION:
		LowerAssignment(node);
		return;
	case SN_CONSTRUCTOR_ACTION:
		if (RemoveTopCv(node.type ? node.type : TypePtr(
				new Type()))->kind == TK_POINTER)
		{
			// A discarded new-expression still allocates and
			// constructs.
			LowerValueExpr(node);
			return;
		}
		MaterializeTemporary(node, "tmpobj", true);
		return;
	case SN_ID_EXPRESSION:
	case SN_MEMBER_EXPRESSION:
		// A discarded class lvalue evaluates to its address.
		if (NodeType(node)->kind == TK_CLASS)
		{
			LowerAddressExpr(node);
			return;
		}
		break;
	case SN_CALL_EXPRESSION:
		if (!IsReferenceType(node.type) &&
		    RemoveTopCv(NodeType(node))->kind == TK_CLASS)
		{
			// A discarded class-valued call still materializes its
			// result object.
			MaterializeClassResult(node, "discard", "");
			return;
		}
		LowerCall(node);
		return;
	case SN_DELETE_EXPRESSION:
	case SN_DELETE_ARRAY:
		LowerDelete(node);
		return;
	case SN_NEW_ARRAY:
		LowerNewArray(node);
		return;
	case SN_NEW_INIT:
		LowerNewInit(node);
		return;
	case SN_UNARY_EXPRESSION:
		if (node.op == OP_INC || node.op == OP_DEC)
		{
			LowerIncDec(node, true);
			return;
		}
		break;
	case SN_POSTFIX_EXPRESSION:
		LowerIncDec(node, false);
		return;
	case SN_CAST_EXPRESSION:
		if (IsVoidType(node.type))
		{
			LowerEffect(*node.children[0]);
			return;
		}
		break;
	case SN_BINARY_EXPRESSION:
		if (node.op == OP_COMMA)
		{
			LowerEffect(*node.children[0]);
			LowerEffect(*node.children[1]);
			return;
		}
		break;
	default:
		break;
	}
	LowerValueExpr(node);
}
