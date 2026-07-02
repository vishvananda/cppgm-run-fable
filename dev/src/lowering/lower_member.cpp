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

string FunctionLowerer::AdjustToBase(const string& address, int hops)
{
	if (hops <= 0)
		return address;
	string hopped = NewTemp();
	Emit(hopped + " = index i8 [projection=base_subobject] " + address +
	     ", 0");
	return hopped;
}

string FunctionLowerer::MemberAddress(const SemNode& node,
                                      bool skip_ref_load)
{
	string base = AdjustToBase(LowerAddressExpr(*node.children[0]),
	                           node.base_hops);
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
	bool guarded = eh_armed_
		? (!temp_cleanups_.empty() || HaveCleanups())
		: (!saved && HaveCleanups());
	if (guarded && !in_cleanup_emission_ && !eh_open_ &&
	    program_.CalleeMayUnwind(callee))
		OpenEhRegion();
	string callee_text = program_.MemberFunctionRef(callee);
	Emit("call void " + callee_text + "(" + arguments + ")");
	in_lifetime_action_ = saved;
	ctor_depth_--;
}

// Constructs the class value of `node` into the object at `dest`:
// constructor actions run in place, class-valued calls write their
// result there, conditionals construct per arm.
void FunctionLowerer::LowerClassInit(const SemNode& node,
                                     const string& dest)
{
	switch (node.kind)
	{
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
			bool saved = in_lifetime_action_;
			in_lifetime_action_ = true;
			LowerCall(*node.children[0]);
			in_lifetime_action_ = saved;
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
		LowerClassInit(*node.children[1], dest);
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(else_label);
		LowerClassInit(*node.children[2], dest);
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
		if (eh_open_)
			CloseEhRegion();
		TempCleanup cleanup;
		cleanup.address = address;
		cleanup.action = call.result_dtor.get();
		temp_cleanups_.push_back(cleanup);
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
	TypePtr cls_type = RemoveTopCv(action.type);
	// An empty object has no bytes to transfer (the PA15 convention):
	// a member-addressed action prints only the member address, while
	// an argument/temporary copy still evaluates the source lvalue.
	bool empty = cls_type->named->class_record &&
		cls_type->named->class_record->is_empty;
	if (empty && member_addressed)
		return;
	string src = LowerAddressExpr(source);
	// A derived source adjusts to the copied base subobject.
	TypePtr source_type = RemoveTopCv(StripRef(source.type));
	if (source_type->kind == TK_CLASS)
	{
		src = AdjustToBase(
			src, BaseClassDistance(source_type->named, cls_type->named));
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
			// must destroy this temporary); close and reopen.
			if (eh_open_)
				CloseEhRegion();
			TempCleanup cleanup;
			cleanup.address = address;
			cleanup.action = dtor;
			temp_cleanups_.push_back(cleanup);
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

void FunctionLowerer::LowerClassLocal(const SemNode& node)
{
	const TypePtr& declared = node.type;
	bool is_array = declared->kind == TK_ARRAY;
	bool any_init = false;
	for (size_t i = 0; i < node.children.size(); i++)
		if (node.children[i]->kind != SN_DESTRUCTOR_ACTION)
			any_init = true;
	// The return-slot-reused local lives in the caller's result
	// storage: no slot, addresses through %ret, no scope cleanup.
	if (nrvo_scope_ && node.entity_scope == nrvo_scope_ &&
	    node.entity_name == nrvo_name_)
	{
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
		return;
	}
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
			if (child.trivial_copy)
			{
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
		case SN_CALL_EXPRESSION:
		case SN_CONDITIONAL_EXPRESSION:
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
	for (size_t i = 0; i < cleanup_scopes_.size(); i++)
		if (!cleanup_scopes_[i].empty())
			return true;
	return false;
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
	ReferenceLabel(eh_end_);
	Terminate("jump ^" + eh_end_);
	OpenBlock(eh_dispatch_);
	EmitTempCleanups(0);
	EmitCleanupsFrom(0);
	Terminate("resume");
	OpenBlock(eh_end_);
}

void FunctionLowerer::OpenEhRegion()
{
	// The unwind runtime declares appear exactly when a dispatch
	// region exists in the emitted program.
	program_.RequireEhRuntime();
	eh_dispatch_ = NewLabel("call_unwind_dispatch");
	eh_end_ = NewLabel("call_unwind_end");
	ReferenceLabel(eh_dispatch_);
	Emit("eh_try ^" + eh_dispatch_);
	eh_open_ = true;
}

// --- full-expression temporaries ---------------------------------------------

// Evaluation-order scan deciding whether the full expression arms
// unwind dispatch: true when some call executes while a caller-owned
// destructible temporary is live. Callee-owned by-value argument
// objects never register a caller cleanup, and a result temporary
// registers only after its producing call, so neither arms alone;
// `skip_own_cleanup` marks a by-value argument position.
bool FunctionLowerer::ScanArmsCleanups(const SemNode& node, bool& live,
                                       bool skip_own_cleanup) const
{
	if (node.kind == SN_DESTRUCTOR_ACTION)
		return false;  // lowers at cleanup time, not in sequence
	if (node.kind == SN_CONSTRUCTOR_ACTION && !node.children.empty())
	{
		// Constructor arguments evaluate before the construction runs.
		const SemNode& call = *node.children[0];
		for (size_t i = 1; i < call.children.size(); i++)
			if (ScanArmsCleanups(*call.children[i], live, false))
				return true;
		bool emits_call = !node.trivial_copy && !node.trivial_init &&
			!node.elided;
		if (emits_call && live)
			return true;
		if (node.needs_dtor && !skip_own_cleanup)
			live = true;
		return false;
	}
	if (node.kind == SN_CALL_EXPRESSION && !node.children.empty())
	{
		const SemNode& callee = *node.children[0];
		TypePtr fn_type;
		if (callee.kind == SN_CALLEE)
			fn_type = callee.type;
		else
		{
			TypePtr through = RemoveTopCv(StripRef(callee.type));
			fn_type = through->kind == TK_POINTER ? through->target
			                                      : through;
		}
		if (ScanArmsCleanups(callee, live, false))
			return true;
		for (size_t i = 1; i < node.children.size(); i++)
		{
			TypePtr param;
			if (fn_type && fn_type->kind == TK_FUNCTION &&
			    i - 1 < fn_type->parameters.size())
				param = fn_type->parameters[i - 1];
			bool callee_owned = param && !IsReferenceType(param) &&
				RemoveTopCv(param)->kind == TK_CLASS;
			if (ScanArmsCleanups(*node.children[i], live, callee_owned))
				return true;
		}
		if (live)
			return true;
		if (node.needs_dtor && !skip_own_cleanup)
			live = true;
		return false;
	}
	bool call_event = node.kind == SN_NEW_INIT ||
		node.kind == SN_NEW_ARRAY || node.kind == SN_DELETE_EXPRESSION ||
		node.kind == SN_DELETE_ARRAY;
	for (size_t i = 0; i < node.children.size(); i++)
		if (ScanArmsCleanups(*node.children[i], live, false))
			return true;
	if (call_event && live)
		return true;
	if (node.kind == SN_CONDITIONAL_EXPRESSION && node.needs_dtor &&
	    !skip_own_cleanup)
		live = true;
	return false;
}

void FunctionLowerer::BeginFullExpression(const SemNode& root)
{
	fe_marks_.push_back(temp_cleanups_.size());
	fe_armed_.push_back(eh_armed_);
	bool live = !temp_cleanups_.empty();
	if (ScanArmsCleanups(root, live, false))
		eh_armed_ = true;
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
