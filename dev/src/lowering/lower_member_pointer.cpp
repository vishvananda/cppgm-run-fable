#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

namespace {

TypePtr StripRef(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

}  // namespace

// PA26 member-pointer lowering: `&C::member` constants (a data member
// is its field offset + 1 with 0 as null; a member function packs
// {address, this-adjustment} into an i128), `.*` / `->*` data-member
// addressing, and the bound member-pointer call pieces.

// PA26 5.3.1p3-p4: a member pointer constant. A data member renders
// its field offset + 1 through a `const` instruction (0 is the null
// value); a member function renders its code address zero-extended
// into the i128 {address, this-adjustment} pair.
LowerValue FunctionLowerer::LowerMemberPointerConstant(const SemNode& node)
{
	const SemNode& member = *node.children[0];
	LowerValue value;
	value.type = RemoveTopCv(node.type);
	if (value.type->target->kind == TK_FUNCTION)
	{
		string fn = program_.MemberFunctionRef(member);
		string address = NewTemp();
		Emit(address + " = addr " + fn);
		string bits = NewTemp();
		Emit(bits + " = copy i64 " + address);
		value.text = NewTemp();
		Emit(value.text + " = convert zext i128 i64 " + bits);
		return value;
	}
	const NamedTypeInfo* owner = member.entity_scope
		? member.entity_scope->entity : 0;
	const ClassInfo* record = owner ? owner->class_record : 0;
	const ClassField* field = record
		? FindClassField(*record, member.entity_name) : 0;
	if (!field)
		throw runtime_error("member pointer names no field");
	value.text = NewTemp();
	Emit(value.text + " = const i64 " + to_string(field->offset + 1));
	return value;
}

// PA26 5.5: the address of `object .* pm` for a data member — the
// stored value is the field offset + 1, so the field projection
// indexes by the decremented value.
string FunctionLowerer::MemberPointerAccessAddress(const SemNode& node)
{
	string base = LowerAddressExpr(*node.children[0]);
	LowerValue pm = LowerValueExpr(*node.children[1]);
	string offset = NewTemp();
	Emit(offset + " = binary sub i64 " + pm.text + ", 1");
	string field = NewTemp();
	Emit(field + " = index i8 [projection=field] " + base + ", " +
	     offset);
	return field;
}

// The loaded value of a data-member access (arrays stay addresses).
LowerValue FunctionLowerer::LowerMemberPointerValue(const SemNode& node)
{
	string address = MemberPointerAccessAddress(node);
	LowerValue value;
	value.type = RemoveTopCv(StripRef(node.type));
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

// The callee of a bound member-pointer call: a constant member pointer
// (`&C::f`, incl. a folded non-type parameter) calls through its
// address directly; otherwise the value's low 64 bits carry the code
// address. When the member pointer's class has a displaced base
// subobject, a runtime value may carry a non-zero this-adjustment in
// its high 64 bits (folded there by 4.11p2 conversions), so the object
// address shifts by it; other classes keep the adjustment-free shape.
string FunctionLowerer::LowerMemberPointerCallee(const SemNode& callee,
                                                 string& object_text)
{
	const SemNode& pm_value = *callee.children[1];
	if (pm_value.kind == SN_UNARY_EXPRESSION && pm_value.has_op &&
	    pm_value.op == OP_AMP && !pm_value.children.empty() &&
	    RemoveTopCv(pm_value.type)->kind == TK_MEMBER_POINTER)
	{
		// The object was adjusted to the member pointer's class; a
		// constant naming a displaced base's member (a folded non-type
		// parameter) still steps down to its owner subobject.
		const SemNode& member = *pm_value.children[0];
		const NamedTypeInfo* pm_class =
			RemoveTopCv(pm_value.type)->named;
		const NamedTypeInfo* owner = member.entity_scope
			? member.entity_scope->entity : 0;
		int hops = 0;
		unsigned long long offset = 0;
		if (pm_class && owner && pm_class != owner &&
		    BaseSubobjectPath(pm_class, owner, hops, offset) ==
		        BP_UNIQUE && offset)
			object_text = AdjustToBaseHops(object_text, hops, offset);
		string address = NewTemp();
		Emit(address + " = addr " +
		     program_.MemberFunctionRef(*pm_value.children[0]));
		return address;
	}
	LowerValue pm = LowerValueExpr(pm_value);
	string bits = NewTemp();
	Emit(bits + " = convert trunc i64 i128 " + pm.text);
	string fn = NewTemp();
	Emit(fn + " = copy ptr " + bits);
	const NamedTypeInfo* pm_class = RemoveTopCv(pm_value.type)->named;
	if (pm_class && ClassHasDisplacedBase(pm_class->class_record))
	{
		string high = NewTemp();
		Emit(high + " = binary ushr i128 " + pm.text + ", 64");
		string delta = NewTemp();
		Emit(delta + " = convert trunc i64 i128 " + high);
		string shifted = NewTemp();
		Emit(shifted + " = index i8 " + object_text + ", " + delta);
		object_text = shifted;
	}
	return fn;
}

// The bound call's `as` signature: the (already adjusted) object
// address leads the declared parameters.
TypePtr FunctionLowerer::MemberPointerCallSignature(const TypePtr& fn_type)
{
	vector<TypePtr> parameters;
	parameters.push_back(MakePointerType(MakeFundamentalType(FT_VOID),
	                                     false, false));
	for (size_t i = 0; i < fn_type->parameters.size(); i++)
		parameters.push_back(fn_type->parameters[i]);
	return MakeFunctionType(fn_type->target, parameters,
	                        fn_type->variadic);
}
