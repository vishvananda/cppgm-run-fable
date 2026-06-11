#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"
#include "sema/const_expr.h"

using std::runtime_error;
using std::to_string;

// PA15 object-model lowering: member addressing, bit-field access,
// class-typed locals and temporaries, constructor/destructor actions,
// and the automatic-object lifetime machinery (cleanups at scope
// exits plus the eh_try unwind-dispatch regions).

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

// The innermost element type of a (possibly array-of) class object.
TypePtr InnerObjectType(const TypePtr& type)
{
	TypePtr inner = type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	return RemoveTopCv(inner);
}

// The unsigned mask of `width` bits.
unsigned long long BitMask(unsigned long long width)
{
	if (width >= 64)
		return ~0ull;
	return (1ull << width) - 1;
}

// Renders a mask constant in the value space of the bit-field's
// declared type (u32 masks print unsigned, i32 masks signed).
string MaskText(unsigned long long mask, const TypePtr& type)
{
	EFundamentalType fundamental = type->kind == TK_ENUM
		? type->named->enum_underlying : type->fundamental;
	return RenderConstValue(
		ConvertConstValue(ConstValue(FT_UNSIGNED_LONG_LONG_INT, mask),
		                  fundamental));
}

}  // namespace

// --- member addressing -----------------------------------------------------

string FunctionLowerer::MemberAddress(const SemNode& node,
                                      bool skip_ref_load)
{
	string base = LowerAddressExpr(*node.children[0]);
	for (int i = 0; i < node.base_hops; i++)
	{
		string hopped = NewTemp();
		Emit(hopped + " = index i8 [projection=base_subobject] " + base +
		     ", 0");
		base = hopped;
	}
	if (!node.name.empty())
	{
		string field = NewTemp();
		Emit(field + " = index i8 [projection=field] " + base + ", " +
		     to_string(node.member_offset));
		base = field;
	}
	if (node.member_ref && !skip_ref_load)
	{
		string referee = NewTemp();
		Emit(referee + " = load ptr " + base);
		base = referee;
	}
	return base;
}

LowerValue FunctionLowerer::LowerMemberValue(const SemNode& node)
{
	if (node.is_bit_field)
		return LowerBitFieldValue(node);
	LowerValue value;
	value.type = RemoveTopCv(StripRef(node.type));
	string address = MemberAddress(node);
	if (value.type->kind == TK_ARRAY)
	{
		value.text = address;
		return value;
	}
	value.text = NewTemp();
	Emit(value.text + " = load " + LowerValueType(value.type) + " " +
	     address);
	return value;
}

LowerValue FunctionLowerer::LowerBitFieldValue(const SemNode& node)
{
	TypePtr unit = RemoveTopCv(node.type);
	// Reads spell the signed view of the storage unit; the value type
	// keeps the declared signedness for later conversions.
	string unit_text;
	switch (TypeSize(unit))
	{
	case 1: unit_text = "i8"; break;
	case 2: unit_text = "i16"; break;
	case 4: unit_text = "i32"; break;
	default: unit_text = "i64"; break;
	}
	bool unsigned_field = LowerUnsignedOps(unit);
	unsigned long long unit_bits = TypeSize(unit) * 8;
	string address = MemberAddress(node);
	string loaded = NewTemp();
	Emit(loaded + " = load " + unit_text + " " + address);
	(void)unsigned_field;
	(void)unit_bits;
	// The canonical read shifts the unit down and masks the width
	// (bit-field reads zero-extend in the reference presentation).
	LowerValue value;
	value.type = unit;
	string shifted = loaded;
	if (node.bit_offset)
	{
		shifted = NewTemp();
		Emit(shifted + " = binary ushr " + unit_text + " " + loaded +
		     ", " + to_string(node.bit_offset));
	}
	value.text = NewTemp();
	Emit(value.text + " = binary and " + unit_text + " " + shifted +
	     ", " + MaskText(BitMask(node.bit_width), unit));
	return value;
}

LowerValue FunctionLowerer::LowerBitFieldAssignment(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	const SemNode& rhs = *node.children[1];
	TypePtr unit = RemoveTopCv(lhs.type);
	string unit_text = LowerValueType(unit);
	unsigned long long mask = BitMask(node.bit_width);
	string value = LowerValueAs(rhs, unit, LCC_OPERAND);
	LowerValue result;
	result.type = unit;
	if (node.bf_plain_store)
	{
		// The first store to a unit inside a constructor writes the
		// masked value directly.
		string masked = NewTemp();
		Emit(masked + " = binary and " + unit_text + " " +
		     MaskText(mask, unit) + ", " + value);
		string shifted = masked;
		if (node.bit_offset)
		{
			shifted = NewTemp();
			Emit(shifted + " = binary shl " + unit_text + " " + masked +
			     ", " + to_string(node.bit_offset));
		}
		string address = MemberAddress(lhs);
		Emit("store " + unit_text + " " + shifted + ", " + address);
		result.text = shifted;
		return result;
	}
	string read_address = MemberAddress(lhs);
	string loaded = NewTemp();
	Emit(loaded + " = load " + unit_text + " " + read_address);
	string cleared = NewTemp();
	Emit(cleared + " = binary and " + unit_text + " " + loaded + ", " +
	     MaskText(~(mask << node.bit_offset), unit));
	string masked = NewTemp();
	Emit(masked + " = binary and " + unit_text + " " +
	     MaskText(mask, unit) + ", " + value);
	string shifted = masked;
	if (node.bit_offset)
	{
		shifted = NewTemp();
		Emit(shifted + " = binary shl " + unit_text + " " + masked +
		     ", " + to_string(node.bit_offset));
	}
	string merged = NewTemp();
	Emit(merged + " = binary or " + unit_text + " " + cleared + ", " +
	     shifted);
	string write_address = MemberAddress(lhs);
	Emit("store " + unit_text + " " + merged + ", " + write_address);
	result.text = merged;
	return result;
}

// A member-lvalue store: the value computes before the address (the
// canonical reference order), reference members bind addresses.
LowerValue FunctionLowerer::LowerMemberAssignment(const SemNode& node)
{
	const SemNode& lhs = *node.children[0];
	const SemNode& rhs = *node.children[1];
	if (node.member_ref)
	{
		string value = LowerAddressExpr(rhs);
		string storage;
		if (lhs.kind == SN_MEMBER_EXPRESSION)
			storage = MemberAddress(lhs, true);
		else
			// A namespace-scope reference binds its pointer object.
			storage = program_.GlobalRef(lhs.entity_scope,
			                             lhs.entity_name);
		Emit("store ptr " + value + ", " + storage);
		LowerValue result;
		result.type = RemoveTopCv(lhs.type);
		result.text = value;
		return result;
	}
	if (lhs.is_bit_field)
		return LowerBitFieldAssignment(node);
	TypePtr type = RemoveTopCv(StripRef(lhs.type));
	string value = LowerValueAs(rhs, type, LCC_OPERAND);
	string storage = MemberAddress(lhs);
	Emit("store " + LowerValueType(type) + " " + value + ", " + storage);
	LowerValue result;
	result.type = type;
	result.text = value;
	return result;
}

// --- class locals and temporaries -------------------------------------------

// The direct constructor call of a declared object: the precomputed
// address is the first argument; the action's own address subtree is
// not relowered.
void FunctionLowerer::LowerConstructorCall(const SemNode& action,
                                           const string& this_text)
{
	// A declared object's action holds [callee, &object, args...]; a
	// temporary's holds [callee, args...].
	const SemNode& call = *action.children[0];
	const SemNode& callee = *call.children[0];
	size_t first_arg = 1;
	if (call.children.size() > 1 &&
	    call.children[1]->kind == SN_UNARY_EXPRESSION &&
	    call.children[1]->op == OP_AMP &&
	    call.children[1]->has_op)
		first_arg = 2;
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	string arguments = this_text;
	for (size_t i = first_arg; i < call.children.size(); i++)
	{
		size_t param_at = i - first_arg + 1;
		TypePtr param = param_at < callee.type->parameters.size()
			? callee.type->parameters[param_at] : TypePtr();
		arguments += ", " + LowerCallArgument(*call.children[i], param);
	}
	string callee_text = program_.MemberFunctionRef(callee);
	Emit("call void " + callee_text + "(" + arguments + ")");
	in_lifetime_action_ = saved;
}

string FunctionLowerer::MaterializeTemporary(const SemNode& action,
                                             const char* kind)
{
	TypePtr type = RemoveTopCv(action.type);
	string slot = AddMatSlot(kind, LowerSlotType(type));
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	LowerConstructorCall(action, address);
	return address;
}

string FunctionLowerer::ClassArrayElement(const string& base,
                                          const LowerValue& index,
                                          const TypePtr& element)
{
	unsigned long long size = TypeSize(RemoveTopCv(element));
	string offset = index.text;
	if (size != 1)
	{
		offset = NewTemp();
		Emit(offset + " = binary mul i64 " + index.text + ", " +
		     to_string(size));
	}
	string result = NewTemp();
	Emit(result + " = index i8 [projection=array_element] " + base +
	     ", " + offset);
	return result;
}

void FunctionLowerer::LowerClassLocal(const SemNode& node)
{
	const TypePtr& declared = node.type;
	string slot = AddSlot(node.entity_scope, node.entity_name,
	                      LowerSlotType(declared));
	bool is_array = declared->kind == TK_ARRAY;
	bool any_init = false;
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i]->kind != SN_DESTRUCTOR_ACTION)
			any_init = true;
	string decl_address;
	if (!is_array && any_init)
	{
		decl_address = NewTemp();
		Emit(decl_address + " = addr $" + slot);
	}
	vector<const SemNode*> dtor_actions;
	bool saved = in_lifetime_action_;
	string aggregate_base;
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		switch (child.kind)
		{
		case SN_CONSTRUCTOR_ACTION:
			if (child.trivial_init)
				break;
			if (is_array && child.has_value)
			{
				// The field-wise aggregate form: elements share one
				// base address and index by byte offset.
				if (aggregate_base.empty())
				{
					aggregate_base = NewTemp();
					Emit(aggregate_base + " = addr $" + slot);
				}
				string element = aggregate_base;
				if (child.value.bits)
				{
					element = NewTemp();
					Emit(element + " = index i8 " + aggregate_base +
					     ", " + to_string(child.value.bits));
				}
				LowerConstructorCall(child, element);
			}
			else if (is_array)
			{
				in_lifetime_action_ = true;
				LowerCall(*child.children[0]);
				in_lifetime_action_ = saved;
			}
			else if (child.children[0]->children.size() > 1 &&
			         child.children[0]->children[1]->kind ==
			             SN_UNARY_EXPRESSION &&
			         child.children[0]->children[1]->op == OP_AMP &&
			         !child.children[0]->children[1]->children.empty() &&
			         child.children[0]->children[1]->children[0]->kind ==
			             SN_MEMBER_EXPRESSION)
			{
				// A subobject-targeted action addresses its own member.
				in_lifetime_action_ = true;
				LowerCall(*child.children[0]);
				in_lifetime_action_ = saved;
			}
			else
				LowerConstructorCall(child, decl_address);
			break;
		case SN_DESTRUCTOR_ACTION:
			dtor_actions.push_back(&child);
			break;
		case SN_EXPRESSION_STATEMENT:
			LowerStatement(child);
			break;
		default:
			throw OutsideBoundary("class object initializer form");
		}
	}
	if (!dtor_actions.empty())
		RegisterCleanup(dtor_actions);
}

// --- lifetime ----------------------------------------------------------------

void FunctionLowerer::PushCleanupScope()
{
	cleanup_scopes_.push_back(vector<vector<const SemNode*>>());
}

void FunctionLowerer::RegisterCleanup(const vector<const SemNode*>& actions)
{
	if (cleanup_scopes_.empty())
		PushCleanupScope();
	cleanup_scopes_.back().push_back(actions);
	program_.RequireEhRuntime();
}

bool FunctionLowerer::HaveCleanups() const
{
	for (size_t i = 0; i < cleanup_scopes_.size(); i++)
		if (!cleanup_scopes_[i].empty())
			return true;
	return false;
}

void FunctionLowerer::EmitCleanupsFrom(size_t from)
{
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	for (size_t scope = cleanup_scopes_.size(); scope-- > from;)
	{
		const vector<vector<const SemNode*>>& groups =
			cleanup_scopes_[scope];
		// Later objects destroy first; one object's recorded actions
		// (array elements) keep their order.
		for (size_t group = groups.size(); group-- > 0;)
			for (size_t i = 0; i < groups[group].size(); i++)
				LowerCall(*groups[group][i]->children[0]);
	}
	in_lifetime_action_ = saved;
}

void FunctionLowerer::PopCleanupScope(bool emit)
{
	if (cleanup_scopes_.empty())
		return;
	if (emit && !blocks_.back().terminated)
	{
		bool saved = in_lifetime_action_;
		in_lifetime_action_ = true;
		const vector<vector<const SemNode*>>& groups =
			cleanup_scopes_.back();
		for (size_t group = groups.size(); group-- > 0;)
			for (size_t i = 0; i < groups[group].size(); i++)
				LowerCall(*groups[group][i]->children[0]);
		in_lifetime_action_ = saved;
	}
	cleanup_scopes_.pop_back();
}

void FunctionLowerer::CloseEhRegion()
{
	if (!eh_open_)
		return;
	eh_open_ = false;
	Emit("eh_end");
	ReferenceLabel(eh_end_);
	Terminate("jump ^" + eh_end_);
	OpenBlock(eh_dispatch_);
	EmitCleanupsFrom(0);
	Terminate("resume");
	OpenBlock(eh_end_);
}
