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

// The selected constructor of an elided (effect-free) construction
// stays odr-used even though the call drops (3.2p3).
static void DemandElidedActionCtor(LowerProgram& program,
                                   const SemNode& action)
{
	if (action.children.empty() ||
	    action.children[0]->kind != SN_CALL_EXPRESSION ||
	    action.children[0]->children.empty())
		return;
	program.DemandElidedCtor(*action.children[0]->children[0]);
}

// String-literal objects inside a dropped (elided) expression still
// exist in the program image (the checked references emit them).
void RegisterDroppedLiterals(LowerProgram& program, const SemNode& node)
{
	if (node.is_string_literal)
		program.StringLiteralRef(node);
	for (size_t i = 0; i < node.children.size(); i++)
		RegisterDroppedLiterals(program, *node.children[i]);
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

string FunctionLowerer::AdjustToBaseHops(const string& address, int hops,
                                         unsigned long long offset)
{
	if (hops <= 0)
		return address;
	string hopped = NewTemp();
	Emit(hopped + " = index i8 [projection=base_subobject] " + address +
	     ", " + to_string(offset));
	return hopped;
}

string FunctionLowerer::AdjustToBase(const string& address,
                                     const NamedTypeInfo* from,
                                     const NamedTypeInfo* to)
{
	int hops = 0;
	unsigned long long offset = 0;
	if (BaseSubobjectPath(from, to, hops, offset) != BP_UNIQUE)
		return address;
	return AdjustToBaseHops(address, hops, offset);
}

string FunctionLowerer::MemberAddress(const SemNode& node,
                                      bool skip_ref_load)
{
	// PA25: a member reached through the lambda-captured this keeps
	// one base-subobject adjustment step (the reference shape).
	const SemNode& object = *node.children[0];
	int hops = node.base_hops;
	if (object.kind == SN_UNARY_EXPRESSION && object.op == OP_STAR &&
	    object.has_op && !object.children.empty() &&
	    object.children[0]->captured_this && hops == 0)
		hops = 1;
	string base = AdjustToBaseHops(LowerAddressExpr(*node.children[0]),
	                               hops, node.base_offset);
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
	// Constructor member-initializer stores stamp the bit facts on the
	// assignment node (with bf_plain_store); ordinary assignments
	// carry them on the member lvalue.
	const SemNode& bits = node.is_bit_field ? node : lhs;
	TypePtr unit = RemoveTopCv(lhs.type);
	string unit_text = LowerValueType(unit);
	unsigned long long mask = BitMask(bits.bit_width);
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
		if (bits.bit_offset)
		{
			shifted = NewTemp();
			Emit(shifted + " = binary shl " + unit_text + " " + masked +
			     ", " + to_string(bits.bit_offset));
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
	     MaskText(~(mask << bits.bit_offset), unit));
	string masked = NewTemp();
	Emit(masked + " = binary and " + unit_text + " " +
	     MaskText(mask, unit) + ", " + value);
	string shifted = masked;
	if (bits.bit_offset)
	{
		shifted = NewTemp();
		Emit(shifted + " = binary shl " + unit_text + " " + masked +
		     ", " + to_string(bits.bit_offset));
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
	if (type->kind == TK_CLASS &&
	    (rhs.kind == SN_CALL_EXPRESSION ||
	     rhs.kind == SN_CONSTRUCTOR_ACTION ||
	     rhs.kind == SN_CONDITIONAL_EXPRESSION))
	{
		// A same-class prvalue initializer constructs the member
		// object directly (copy elision).
		string storage = MemberAddress(lhs);
		LowerClassInit(rhs, storage);
		LowerValue result;
		result.type = type;
		result.text = storage;
		return result;
	}
	if (type->kind == TK_CLASS)
	{
		// Value-initialization zero-fills the object representation.
		string storage = MemberAddress(lhs);
		unsigned long long size = TypeSize(type);
		string width;
		switch (size)
		{
		case 1: width = "i8"; break;
		case 2: width = "i16"; break;
		case 4: width = "i32"; break;
		case 8: width = "i64"; break;
		default:
			break;
		}
		if (width.empty())
			Emit("zeroinit " + to_string(size) + "x" +
			     to_string(TypeAlignment(type)) + " " + storage);
		else
			Emit("store " + width + " 0, " + storage);
		LowerValue result;
		result.type = type;
		result.text = storage;
		return result;
	}
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
	// 8.5p7: a value-initialized temporary zero-fills its storage
	// before the (non-user-provided) default constructor runs; the
	// addressed member/object form handles this in its own path.
	if (action.value_zero_fill && !action.ctor_addressed &&
	    !this_text.empty())
	{
		unsigned long long size = action.value.bits;
		string width;
		switch (size)
		{
		case 1: width = "i8"; break;
		case 2: width = "i16"; break;
		case 4: width = "i32"; break;
		case 8: width = "i64"; break;
		default:
			break;
		}
		if (size > 0)
		{
			if (width.empty())
				Emit("zeroinit " + to_string(size) + "x1 " +
				     this_text);
			else
				Emit("store " + width + " 0, " + this_text);
		}
	}
	// A declared object's action holds [callee, &object, args...]; a
	// temporary's holds [callee, args...].
	const SemNode& call = *action.children[0];
	const SemNode& callee = *call.children[0];
	size_t first_arg = action.ctor_addressed ? 2 : 1;
	ctor_depth_++;
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
	// A may-unwind construction runs under a dispatch region once
	// destructible objects (argument temporaries, live locals) need
	// cleanup on unwind.
	bool guarded = !temp_cleanups_.empty() ||
		(!saved && HaveCleanups());
	if (guarded && !in_cleanup_emission_ && !eh_open_ &&
	    !suppress_eh_regions_ && program_.CalleeMayUnwind(callee))
		OpenEhRegion();
	string callee_text = program_.MemberFunctionRef(callee);
	Emit("call void " + callee_text + "(" + arguments + ")");
	in_lifetime_action_ = saved;
	ctor_depth_--;
}

// PA24/PA25 closure construction into `dest`, one child per closure
// field in order. A reference field (by-reference capture) stores the
// child lvalue's address, the __this pointer field stores the
// captured pointer value, and a by-copy field copies the child's
// value (class captures construct in place). The field offsets are
// sema's layout facts: child i pairs with the closure class record's
// field i (capture order).
void FunctionLowerer::LowerClosureInit(const SemNode& node,
                                       const string& dest)
{
	const ClassInfo* cls = program_.ProgramClass(node.type->named);
	if (!cls || cls->fields.size() < node.children.size())
		throw runtime_error("closure class record missing");
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		const ClassField& field = cls->fields[i];
		string target = dest;
		if (field.offset)
		{
			target = NewTemp();
			Emit(target + " = index i8 " + dest + ", " +
			     to_string(field.offset));
		}
		if (IsReferenceType(field.type) || field.captured_this)
		{
			string value = child.category == VC_LVALUE
				? LowerAddressExpr(child)
				: LowerValueExpr(child).text;
			Emit("store ptr " + value + ", " + target);
			continue;
		}
		TypePtr bare = RemoveTopCv(field.type);
		if (bare->kind == TK_CLASS)
		{
			LowerClassInit(child, target);
			continue;
		}
		string value = LowerValueAs(child, bare, LCC_INIT);
		Emit("store " + LowerValueType(bare) + " " + value + ", " +
		     target);
	}
}

// PA25 18.9: fills the initializer_list record at `dest`: the
// elements store into a fresh backing array whose address and count
// become the record's {begin, size} fields.
void FunctionLowerer::LowerInitializerListInit(const SemNode& node,
                                               const string& dest)
{
	const ClassInfo* cls = program_.ProgramClass(node.type->named);
	if (!cls || cls->fields.size() < 2 ||
	    cls->fields[0].type->kind != TK_POINTER)
		throw runtime_error("initializer_list record missing");
	TypePtr element = RemoveTopCv(cls->fields[0].type->target);
	unsigned long long size = TypeSize(element);
	unsigned long long count = node.children.size();
	TypePtr array = MakeArrayType(element, true, count ? count : 1);
	string backing = AddMatSlot("initlist", LowerSlotType(array));
	string base = NewTemp();
	Emit(base + " = addr $" + backing);
	for (size_t i = 0; i < node.children.size(); i++)
	{
		if (element->kind == TK_CLASS)
		{
			// A class element constructs (or copies) in place; the
			// shipped contract exercises the trivially-destructible
			// subset (backing-array element destruction is PA26+).
			string target = base;
			if (i)
			{
				target = NewTemp();
				Emit(target + " = index i8 " + base + ", " +
				     to_string(i * size));
			}
			LowerClassInit(*node.children[i], target);
			continue;
		}
		// The element value evaluates before its target address.
		string value = LowerValueAs(*node.children[i], element,
		                            LCC_INIT);
		string target = base;
		if (i)
		{
			target = NewTemp();
			Emit(target + " = index i8 " + base + ", " +
			     to_string(i * size));
		}
		Emit("store " + LowerValueType(element) + " " + value + ", " +
		     target);
	}
	Emit("store ptr " + base + ", " + dest);
	string size_address = NewTemp();
	Emit(size_address + " = index i8 " + dest + ", " +
	     to_string(cls->fields[1].offset));
	Emit("store i64 " + to_string(count) + ", " + size_address);
}

// Constructs the class value of `node` into the object at `dest`:
// constructor actions run in place, class-valued calls write their
// result there, conditionals construct per arm.
void FunctionLowerer::LowerClassInit(const SemNode& node,
                                     const string& dest)
{
	switch (node.kind)
	{
	case SN_CLOSURE_INIT:
		LowerClosureInit(node, dest);
		return;
	case SN_BRACED_INIT_LIST:
		// PA25 8.5.4: a braced initializer_list value fills the
		// {begin, size} record from its materialized backing array.
		LowerInitializerListInit(node, dest);
		return;
	case SN_CONSTRUCTOR_ACTION:
		if (node.trivial_copy)
			LowerTrivialCopyAction(node, dest);
		else if (node.ctor_addressed &&
		         node.children[0]->children.size() > 1 &&
		         !node.children[0]->children[1]->children.empty() &&
		         node.children[0]->children[1]->children[0]->kind ==
		             SN_MEMBER_EXPRESSION)
		{
			// A subobject-targeted action addresses its own member.
			// See LowerClassLocal: armed cleanups put a may-unwind
			// member construction under its own dispatch region.
			bool guard = !in_cleanup_emission_ && !eh_open_ &&
				!suppress_eh_regions_ &&
				(eh_armed_ || !temp_cleanups_.empty() ||
				 HaveCleanups()) &&
				program_.CalleeMayUnwind(
					*node.children[0]->children[0]);
			if (guard)
				OpenEhRegion();
			bool saved = in_lifetime_action_;
			in_lifetime_action_ = true;
			LowerCall(*node.children[0]);
			in_lifetime_action_ = saved;
			if (eh_open_)
				CloseEhRegion();
		}
		else
			LowerConstructorCall(node, dest);
		return;
	case SN_CALL_EXPRESSION:
		MaterializeClassResult(node, "tmpobj", dest);
		return;
	case SN_CONDITIONAL_EXPRESSION:
	{
		string then_label = NewLabel("condobj_then");
		string else_label = NewLabel("condobj_else");
		string end_label = NewLabel("condobj_end");
		BranchOnValue(*node.children[0], then_label, else_label);
		OpenBlock(then_label);
		cond_arm_depth_++;
		OpenSegmentRegion(*node.children[1]);
		LowerClassInit(*node.children[1], dest);
		if (eh_open_)
			CloseEhRegion();
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(else_label);
		OpenSegmentRegion(*node.children[2]);
		LowerClassInit(*node.children[2], dest);
		if (eh_open_)
			CloseEhRegion();
		cond_arm_depth_--;
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(end_label);
		return;
	}
	default:
		break;
	}
	// A class lvalue/xvalue source without a wrapping constructor (the
	// trivial direct transfer): raw object copy.
	TypePtr bare = RemoveTopCv(StripRef(node.type));
	if (bare->kind != TK_CLASS)
		throw OutsideBoundary("class object initializer form");
	string src = LowerAddressExpr(node);
	if (bare->named->class_record && bare->named->class_record->is_empty)
		return;
	Emit("copyobj " + LowerObjSpan(bare) + " " + src + ", " + dest);
}

// A class-valued call's result object: written into `dest` when
// given, else materialized in a fresh `kind` slot. Returns the object
// address text.
string FunctionLowerer::MaterializeClassResult(const SemNode& call,
                                               const char* kind,
                                               const string& dest)
{
	TypePtr bare = RemoveTopCv(StripRef(call.type));
	string address = dest;
	if (address.empty())
	{
		string slot = AddMatSlot(kind, LowerSlotType(bare));
		address = NewTemp();
		Emit(address + " = addr $" + slot);
	}
	if (call.kind == SN_CONDITIONAL_EXPRESSION)
		LowerClassInit(call, address);
	else if (LowerClassReturnDirect(bare))
	{
		LowerValue result = LowerCall(call);
		Emit("copyobj " + LowerObjSpan(bare) + " " + result.text + ", " +
		     address);
	}
	else
		LowerCall(call, address);
	if (call.needs_dtor && dest.empty())
	{
		// The materialized result is a destructible temporary of the
		// enclosing full expression; the binder pinned its resolved
		// destructor action on the node.
		if (!call.result_dtor)
			throw runtime_error("class result temporary without a "
			                    "resolved destructor");
		// Registration invalidates the open dispatch; the region
		// closes and immediately reopens on the widened cleanup set.
		// A conditional arm's temporary stays unregistered (the
		// reference does not track its cleanup across the join).
		bool reopen = eh_open_;
		if (eh_open_)
			CloseEhRegion();
		if (!cond_arm_depth_)
		{
			TempCleanup cleanup;
			cleanup.address = address;
			cleanup.action = call.result_dtor.get();
			temp_cleanups_.push_back(cleanup);
			if (reopen)
				OpenEhRegion();
		}
	}
	return address;
}

// A trivial copy/move construction: the source object's bytes copy
// directly into the destination (the PA16 direct value-transfer form).
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

string FunctionLowerer::MaterializeTemporary(const SemNode& action,
                                             const char* kind,
                                             bool register_cleanup)
{
	TypePtr type = RemoveTopCv(action.type);
	string slot = AddMatSlot(kind, LowerSlotType(type));
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	if (action.trivial_copy)
		LowerTrivialCopyAction(action, address);
	else
		LowerConstructorCall(action, address);
	if (action.needs_dtor && register_cleanup)
	{
		const SemNode* dtor = 0;
		for (size_t i = 0; i < action.children.size(); i++)
			if (action.children[i]->kind == SN_DESTRUCTOR_ACTION)
				dtor = action.children[i].get();
		if (dtor)
		{
			// Registration invalidates the open dispatch (it now
			// must destroy this temporary); close and immediately
			// reopen on the widened cleanup set. A conditional arm's
			// temporary stays unregistered.
			bool reopen = eh_open_;
			if (eh_open_)
				CloseEhRegion();
			if (!cond_arm_depth_)
			{
				TempCleanup cleanup;
				cleanup.address = address;
				cleanup.action = dtor;
				temp_cleanups_.push_back(cleanup);
				if (reopen)
					OpenEhRegion();
			}
		}
	}
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

// The checked references drop an overloaded-operator (or user
// conversion, PA23) call that initializes an empty class object (the
// callee stays odr-used and prints on its ordinary terms; literal
// objects in the dropped arguments still exist).
bool FunctionLowerer::ElideEmptyOperatorInit(const SemNode& node,
                                             const SemNode& child)
{
	// A trivial copy of an operator/conversion result reads through
	// to the producing call (the copy itself moves no data).
	const SemNode* producer = &child;
	if (child.kind == SN_CONSTRUCTOR_ACTION && child.trivial_copy &&
	    !child.children.empty())
	{
		const SemNode& call = *child.children[0];
		size_t first_arg = child.ctor_addressed ? 2 : 1;
		if (call.children.size() > first_arg)
			producer = call.children[first_arg].get();
	}
	if (producer->kind != SN_CALL_EXPRESSION ||
	    (!producer->from_operator && !producer->user_conversion) ||
	    producer->children.empty() ||
	    producer->children[0]->kind != SN_CALLEE ||
	    RemoveTopCv(node.type)->kind != TK_CLASS ||
	    !RemoveTopCv(node.type)->named->class_record ||
	    !RemoveTopCv(node.type)->named->class_record->is_empty)
		return false;
	const SemNode& callee = *producer->children[0];
	if (!producer->from_operator)
	{
		// PA23: a conversion elides only when its bound body published
		// the no-observable-work fact (sema owns the body analysis; a
		// conversion with real work keeps its calls).
		const SemNode* body = program_.MemberDefinitionFor(callee);
		if (!body || !body->conversion_no_work)
			return false;
	}
	if (callee.is_method || callee.special != SF_NONE)
		program_.MemberFunctionRef(callee);
	else
		program_.FunctionRef(callee.entity_scope, callee.entity_name,
		                     callee.type, callee.fn_spec);
	RegisterDroppedLiterals(program_, child);
	return true;
}

// The return-slot-reused local lives in the caller's result storage:
// no slot, addresses through %ret, no scope cleanup. False when the
// local is not the NRVO object.
bool FunctionLowerer::LowerNrvoLocal(const SemNode& node)
{
	if (!nrvo_scope_ || node.entity_scope != nrvo_scope_ ||
	    node.entity_name != nrvo_name_)
		return false;
	address_aliases_[std::make_pair(
		(const void*)node.entity_scope, node.entity_name)] = "%ret";
	nrvo_active_ = true;
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		if (child.kind == SN_DESTRUCTOR_ACTION)
			continue;  // ownership transfers with the result
		if (child.kind == SN_CONSTRUCTOR_ACTION)
		{
			if (!child.trivial_init)
				LowerClassInit(child, "%ret");
		}
		else if (child.kind == SN_EXPRESSION_STATEMENT)
			LowerStatement(child);
		else
			LowerClassInit(child, "%ret");
	}
	return true;
}

void FunctionLowerer::LowerClassLocal(const SemNode& node)
{
	const TypePtr& declared = node.type;
	bool is_array = declared->kind == TK_ARRAY;
	bool any_init = false;
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i]->kind != SN_DESTRUCTOR_ACTION)
			any_init = true;
	if (LowerNrvoLocal(node))
		return;
	string slot = AddSlot(node.entity_scope, node.entity_name,
	                      LowerSlotType(declared));
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
			if (child.elided)
			{
				DemandElidedActionCtor(program_, child);
				break;
			}
			if (child.trivial_copy)
			{
				if (ElideEmptyOperatorInit(node, child))
					break;
				LowerTrivialCopyAction(child, decl_address);
				break;
			}
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
			else if (child.ctor_addressed &&
			         child.children[0]->children.size() > 1 &&
			         !child.children[0]->children[1]->children.empty() &&
			         child.children[0]->children[1]->children[0]->kind ==
			             SN_MEMBER_EXPRESSION)
			{
				// A subobject-targeted action addresses its own member.
				// A may-unwind member construction runs under its own
				// dispatch region while cleanups are armed, closed as
				// the subobject comes alive.
				bool guard = !in_cleanup_emission_ && !eh_open_ &&
					!suppress_eh_regions_ &&
					(eh_armed_ || !temp_cleanups_.empty() ||
					 HaveCleanups()) &&
					program_.CalleeMayUnwind(
						*child.children[0]->children[0]);
				if (guard)
					OpenEhRegion();
				in_lifetime_action_ = true;
				LowerCall(*child.children[0]);
				in_lifetime_action_ = saved;
				if (eh_open_)
					CloseEhRegion();
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
		case SN_CALL_EXPRESSION:
		case SN_CONDITIONAL_EXPRESSION:
		case SN_CLOSURE_INIT:
		case SN_BRACED_INIT_LIST:
			if (ElideEmptyOperatorInit(node, child))
				break;
			// A class prvalue initializer constructs the declared
			// object directly (copy elision).
			LowerClassInit(child, decl_address);
			break;
		default:
			throw OutsideBoundary("class object initializer form");
		}
	}
	if (!dtor_actions.empty())
		pending_cleanup_ = dtor_actions;
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
}

bool FunctionLowerer::HaveCleanups() const
{
	// Parameter cleanups run at normal exits but do not arm unwind
	// dispatch on their own (the reference protects calls only over
	// local-object cleanups).
	size_t groups = 0;
	for (size_t i = 0; i < cleanup_scopes_.size(); i++)
		groups += cleanup_scopes_[i].size();
	return groups > param_cleanup_count_;
}

void FunctionLowerer::EmitCleanupsFrom(size_t from)
{
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	bool saved_emission = in_cleanup_emission_;
	in_cleanup_emission_ = true;
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
	in_cleanup_emission_ = saved_emission;
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
		bool saved_emission = in_cleanup_emission_;
		in_cleanup_emission_ = true;
		const vector<vector<const SemNode*>>& groups =
			cleanup_scopes_.back();
		for (size_t group = groups.size(); group-- > 0;)
			for (size_t i = 0; i < groups[group].size(); i++)
				LowerCall(*groups[group][i]->children[0]);
		in_cleanup_emission_ = saved_emission;
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
	// A region on an already-emitted dispatch closes in place; only a
	// fresh dispatch block splits the flow around its emission.
	if (eh_reused_)
		return;
	eh_dispatch_cache_[eh_pending_signature_] = eh_dispatch_;
	ReferenceLabel(eh_end_);
	Terminate("jump ^" + eh_end_);
	OpenBlock(eh_dispatch_);
	// Inside a try, the dispatch re-arms the try's handler set, runs
	// the pending cleanups, and routes to the handler entry; outside,
	// it resumes the unwind.
	size_t enclosing = eh_contexts_.size();
	while (enclosing > 0 && eh_contexts_[enclosing - 1].is_catch)
		enclosing--;
	if (enclosing > 0)
	{
		EhContext& outer = eh_contexts_[enclosing - 1];
		EmitTryMarkers(outer);
		Emit("eh_cleanup");
		EmitTempCleanups(0);
		EmitCleanupsFrom(outer.cleanup_depth);
		Emit("eh_end");
		Emit("eh_end");
		ReferenceLabel(outer.entry_label);
		Terminate("jump ^" + outer.entry_label);
	}
	else
	{
		EmitTempCleanups(0);
		EmitCleanupsFrom(0);
		Terminate("resume");
	}
	OpenBlock(eh_end_);
}

// The content identity of the dispatch a region opened now would
// emit: the live temporary cleanups then every scope's registered
// actions, in emission order.
string FunctionLowerer::CleanupSignature() const
{
	string signature;
	// A dispatch inside a try routes to that try's handler entry, so
	// its content keys on the try context too.
	for (size_t i = eh_contexts_.size(); i-- > 0;)
		if (!eh_contexts_[i].is_catch)
		{
			signature += "try:" + eh_contexts_[i].dispatch_label + ";";
			break;
		}
	char buffer[32];
	for (size_t i = temp_cleanups_.size(); i-- > 0;)
	{
		snprintf(buffer, sizeof(buffer), "%p",
		         (const void*)temp_cleanups_[i].action);
		signature += "t" + temp_cleanups_[i].address + ":" + buffer +
			";";
	}
	for (size_t scope = cleanup_scopes_.size(); scope-- > 0;)
	{
		const vector<vector<const SemNode*>>& groups =
			cleanup_scopes_[scope];
		for (size_t group = groups.size(); group-- > 0;)
			for (size_t i = 0; i < groups[group].size(); i++)
			{
				snprintf(buffer, sizeof(buffer), "%p",
				         (const void*)groups[group][i]);
				signature += string("c") + buffer + ";";
			}
	}
	return signature;
}

void FunctionLowerer::OpenEhRegion(const char* end_prefix)
{
	string signature = CleanupSignature();
	map<string, string>::const_iterator found =
		eh_dispatch_cache_.find(signature);
	if (found != eh_dispatch_cache_.end())
	{
		eh_dispatch_ = found->second;
		eh_reused_ = true;
	}
	else
	{
		eh_dispatch_ = NewLabel("call_unwind_dispatch");
		eh_end_ = NewLabel(end_prefix);
		eh_reused_ = false;
		eh_pending_signature_ = signature;
	}
	ReferenceLabel(eh_dispatch_);
	Emit("eh_try ^" + eh_dispatch_);
	eh_open_ = true;
}

// A straight-line segment (full expression, conditional arm) opens
// its region up front when it will run a call with cleanups armed:
// destructible temporaries protect every call of the segment, armed
// locals only calls the unwind analysis cannot prove non-throwing.
void FunctionLowerer::OpenSegmentRegion(const SemNode& root)
{
	if (eh_open_ || in_cleanup_emission_ || suppress_eh_regions_ ||
	    in_lifetime_action_)
		return;
	if (eh_armed_ || !temp_cleanups_.empty())
	{
		if (SegmentContainsCall(root, false))
			OpenEhRegion();
		return;
	}
	if (HaveCleanups() && SegmentContainsCall(root, true))
		OpenEhRegion();
}

// True when the segment itself runs a call (`may_unwind_only`: a call
// the unwind analysis cannot prove non-throwing): conditionally
// evaluated arms are their own segments, so only a conditional's
// condition counts here. Destructor actions lower at cleanup time,
// never in sequence.
bool FunctionLowerer::SegmentContainsCall(const SemNode& node,
                                          bool may_unwind_only)
{
	switch (node.kind)
	{
	case SN_DESTRUCTOR_ACTION:
		return false;
	case SN_THROW:
		// The throw lowering manages its own dispatch regions.
		return false;
	case SN_CALL_EXPRESSION:
		// A std::type_info comparison folds to a pointer compare.
		if (IsTypeInfoComparison(node))
			break;
		if (!node.children.empty())
		{
			const SemNode& callee = *node.children[0];
			if (!may_unwind_only || callee.kind != SN_CALLEE ||
			    program_.CalleeMayUnwind(callee))
				return true;
		}
		break;
	case SN_NEW_INIT:
	case SN_NEW_ARRAY:
	case SN_DELETE_EXPRESSION:
	case SN_DELETE_ARRAY:
		return true;
	case SN_CONSTRUCTOR_ACTION:
		if (!node.trivial_copy && !node.trivial_init && !node.elided &&
		    !node.children.empty() &&
		    !node.children[0]->children.empty())
		{
			if (!may_unwind_only ||
			    program_.CalleeMayUnwind(*node.children[0]->children[0]))
				return true;
		}
		break;
	case SN_CONDITIONAL_EXPRESSION:
		return !node.children.empty() &&
			SegmentContainsCall(*node.children[0], may_unwind_only);
	case SN_BINARY_EXPRESSION:
		if (node.op == OP_LAND || node.op == OP_LOR)
			return SegmentContainsCall(*node.children[0],
			                           may_unwind_only);
		break;
	default:
		break;
	}
	for (size_t i = 0; i < node.children.size(); i++)
		if (SegmentContainsCall(*node.children[i], may_unwind_only))
			return true;
	return false;
}

// --- full-expression temporaries ---------------------------------------------

// Evaluation-order scan deciding whether the full expression arms
// unwind dispatch: true when some call executes while a destructible
// temporary is live. Callee-owned by-value argument objects count
// too - the reference protects the calls of their full expression
// even though no caller cleanup registers for them.
bool FunctionLowerer::ScanArmsCleanups(const SemNode& node,
                                       bool& live) const
{
	if (node.kind == SN_DESTRUCTOR_ACTION)
		return false;  // lowers at cleanup time, not in sequence
	if (node.kind == SN_CONSTRUCTOR_ACTION && !node.children.empty())
	{
		// Constructor arguments evaluate before the construction runs.
		const SemNode& call = *node.children[0];
		for (size_t i = 1; i < call.children.size(); i++)
			if (ScanArmsCleanups(*call.children[i], live))
				return true;
		bool emits_call = !node.trivial_copy && !node.trivial_init &&
			!node.elided;
		if (emits_call && live)
			return true;
		if (node.needs_dtor)
			live = true;
		return false;
	}
	if (node.kind == SN_CALL_EXPRESSION && !node.children.empty())
	{
		if (ScanArmsCleanups(*node.children[0], live))
			return true;
		for (size_t i = 1; i < node.children.size(); i++)
			if (ScanArmsCleanups(*node.children[i], live))
				return true;
		if (live)
			return true;
		if (node.needs_dtor)
			live = true;
		return false;
	}
	if (node.kind == SN_CONDITIONAL_EXPRESSION &&
	    node.children.size() == 3)
	{
		// The arms evaluate exclusively: one arm's temporary is never
		// live during the other arm's calls. The conditional's own
		// result temporary never registers (its cleanup is dropped),
		// so it does not arm later calls either.
		if (ScanArmsCleanups(*node.children[0], live))
			return true;
		bool live_then = live;
		bool live_else = live;
		if (ScanArmsCleanups(*node.children[1], live_then) ||
		    ScanArmsCleanups(*node.children[2], live_else))
			return true;
		live = live_then || live_else;
		return false;
	}
	if (node.kind == SN_BINARY_EXPRESSION &&
	    (node.op == OP_LAND || node.op == OP_LOR) &&
	    node.children.size() == 2)
	{
		if (ScanArmsCleanups(*node.children[0], live))
			return true;
		bool live_rhs = live;
		if (ScanArmsCleanups(*node.children[1], live_rhs))
			return true;
		live = live_rhs;
		return false;
	}
	bool call_event = node.kind == SN_NEW_INIT ||
		node.kind == SN_NEW_ARRAY || node.kind == SN_DELETE_EXPRESSION ||
		node.kind == SN_DELETE_ARRAY || node.kind == SN_THROW;
	for (size_t i = 0; i < node.children.size(); i++)
		if (ScanArmsCleanups(*node.children[i], live))
			return true;
	if (call_event && live)
		return true;
	if (node.kind == SN_CONDITIONAL_EXPRESSION && node.needs_dtor)
		live = true;
	return false;
}

void FunctionLowerer::BeginFullExpression(const SemNode& root,
                                          bool open_segment)
{
	fe_marks_.push_back(temp_cleanups_.size());
	fe_armed_.push_back(eh_armed_);
	bool live = !temp_cleanups_.empty();
	if (ScanArmsCleanups(root, live))
		eh_armed_ = true;
	if (open_segment)
		OpenSegmentRegion(root);
}

// Destruction order is the reverse of construction; the records stay
// live for the closing dispatch and are dropped afterwards.
void FunctionLowerer::EndFullExpression()
{
	size_t mark = fe_marks_.back();
	fe_marks_.pop_back();
	if (temp_cleanups_.size() > mark && !blocks_.back().terminated)
		EmitTempCleanups(mark);
	CloseEhRegion();
	temp_cleanups_.resize(mark);
	eh_armed_ = fe_armed_.back() != 0;
	fe_armed_.pop_back();
}

void FunctionLowerer::EmitTempCleanups(size_t from)
{
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	bool saved_emission = in_cleanup_emission_;
	in_cleanup_emission_ = true;
	for (size_t i = temp_cleanups_.size(); i-- > from;)
	{
		const SemNode& call = *temp_cleanups_[i].action->children[0];
		Emit("call void " +
		     program_.MemberFunctionRef(*call.children[0]) + "(" +
		     temp_cleanups_[i].address + ")");
	}
	in_cleanup_emission_ = saved_emission;
	in_lifetime_action_ = saved;
}

// --- PA17 polymorphic lowering ----------------------------------------------

// SN_VPOINTER_STORE: the constructor/destructor stores the address of
// the class's first vtable function slot (vtable + 16, past the
// offset-to-top and RTTI entries) into the object's vpointer.
void FunctionLowerer::LowerVPointerStore(const SemNode& node)
{
	string self = LowerValueExpr(*node.children[0]).text;
	const ClassInfo* cls = RemoveTopCv(node.type)->named->class_record;
	if (!cls)
		throw runtime_error("vpointer store without a class record");
	string table = NewTemp();
	Emit(table + " = addr " + program_.VTableRef(cls));
	string entry = NewTemp();
	Emit(entry + " = index i8 " + table + ", 16");
	Emit("store ptr " + entry + ", " + self);
}

// The deleting destructor entry (D0) shares the complete destructor's
// body and then frees the object through the usual deallocation
// function (5.3.5, no operator-delete overloading in this subset).
void FunctionLowerer::EmitDeletingEpilogue()
{
	const Scope* global = info_.scope;
	while (global && global->parent)
		global = global->parent;
	const ScopeBinding* binding =
		global ? FindOwnBinding(*global, "operator delete") : 0;
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("deleting destructor without a declared "
		                    "operator delete");
	string fn = program_.FunctionRef(global, "operator delete",
	                                 binding->type);
	string self = NewTemp();
	Emit(self + " = load ptr $this");
	Emit("call void " + fn + "(" + self + ")");
}

// The explicit `as (...) -> ...` signature of an indirect or
// vtable-dispatched call, spelled from the lowered ABI boundary.
string FunctionLowerer::IndirectCallSignature(const TypePtr& fn_type)
{
	string return_text;
	bool indirect_result = LowerAbiReturn(fn_type->target, return_text);
	string signature;
	size_t at = 0;
	if (indirect_result)
	{
		signature = "%arg0 : ptr [pass=indirect_result]";
		at = 1;
	}
	for (size_t i = 0; i < fn_type->parameters.size(); i++, at++)
	{
		string param_text;
		string pass;
		LowerAbiParameter(fn_type->parameters[i], param_text, pass);
		signature += (at ? ", " : "") + string("%arg") + to_string(at) +
			" : " + param_text;
		if (!pass.empty())
			signature += " [pass=" + pass + "]";
	}
	string text = " as (" + signature + ") -> " + return_text;
	if (fn_type->variadic)
		text += " [arity=variadic]";
	return text;
}
