#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// PA16 allocation-expression lowering: scalar and array new over
// explicit storage, the count-header convention for class arrays, and
// delete / delete[] with element destruction.

// Scalar new of a class object: the allocation call provides the
// object address; the constructor runs on it (a non-throwing
// allocation branches around construction on null).
LowerValue FunctionLowerer::LowerNewConstruction(const SemNode& node)
{
	const SemNode& call = *node.children[0];
	LowerValue address = LowerValueExpr(*call.children[1]);
	string init_label;
	string end_label;
	if (node.null_pointer)
	{
		string c = NewTemp();
		Emit(c + " = cmp ne ptr " + address.text + ", 0");
		init_label = NewLabel("new_init");
		end_label = NewLabel("new_end");
		ReferenceLabel(init_label);
		ReferenceLabel(end_label);
		Terminate("branch " + c + ", ^" + init_label + ", ^" +
		          end_label);
		OpenBlock(init_label);
	}
	if (!node.trivial_init)
	{
		bool saved = in_lifetime_action_;
		in_lifetime_action_ = true;
		const SemNode& callee = *call.children[0];
		string arguments = address.text;
		for (size_t i = 2; i < call.children.size(); i++)
		{
			TypePtr param = i - 1 < callee.type->parameters.size()
				? callee.type->parameters[i - 1] : TypePtr();
			arguments += ", " +
				LowerCallArgument(*call.children[i], param);
		}
		Emit("call void " + program_.MemberFunctionRef(callee) + "(" +
		     arguments + ")");
		in_lifetime_action_ = saved;
	}
	if (!init_label.empty())
	{
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(end_label);
	}
	LowerValue value;
	value.type = RemoveTopCv(node.type);
	value.text = address.text;
	return value;
}

// Array new: size computation (count * element-size plus the 8-byte
// count header for class elements), the allocation call, the stored
// count, and the per-element construction loop.
LowerValue FunctionLowerer::LowerNewArray(const SemNode& node)
{
	const SemNode& callee = *node.children[0];
	const SemNode& count = *node.children[1];
	const SemNode* ctor = 0;
	for (size_t i = 2; i < node.children.size(); i++)
		if (node.children[i]->kind == SN_CONSTRUCTOR_ACTION)
			ctor = node.children[i].get();
	unsigned long long elem_size = node.value.bits;
	unsigned long long header = node.member_offset;
	TypePtr ulong = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	bool runtime = !count.has_value;
	string alloc_arg;
	string size_slot;
	if (!runtime)
	{
		LowerValue total;
		total.imm_int = true;
		total.type = MakeFundamentalType(FT_INT);
		total.value = ConstValue(FT_INT,
		                         count.value.bits * elem_size + header);
		total.text = RenderConstValue(total.value);
		total = ConvertValue(total, ulong, LCC_INIT);
		alloc_arg = total.text;
	}
	else
	{
		LowerValue counted = LowerValueExpr(count);
		string current = counted.text;
		string count_type = LowerValueType(counted.type);
		if (elem_size > 1)
		{
			string scaled = NewTemp();
			Emit(scaled + " = binary mul " + count_type + " " + current +
			     ", " + to_string(elem_size));
			current = scaled;
		}
		if (header)
		{
			string padded = NewTemp();
			Emit(padded + " = binary add " + count_type + " " + current +
			     ", " + to_string(header));
			current = padded;
		}
		LowerValue size_value;
		size_value.text = current;
		size_value.type = counted.type;
		size_value = ConvertValue(size_value, ulong, LCC_INIT);
		size_slot = AddMatSlot("array_new_size", "i64");
		Emit("store i64 " + size_value.text + ", $" + size_slot);
		alloc_arg = size_value.text;
	}
	string fn = program_.FunctionRef(callee.entity_scope,
	                                 callee.entity_name, callee.type);
	string result = NewTemp();
	Emit(result + " = call ptr " + fn + "(" + alloc_arg + ")");
	string data = result;
	string runtime_count;
	string runtime_size;
	if (runtime && (header || node.trivial_init))
	{
		runtime_size = NewTemp();
		Emit(runtime_size + " = load i64 $" + size_slot);
	}
	if (header)
	{
		data = NewTemp();
		Emit(data + " = index i8 " + result + ", " + to_string(header));
		if (!runtime)
		{
			string stored = NewTemp();
			Emit(stored + " = const i64 " +
			     to_string(count.value.bits));
			Emit("store i64 " + stored + ", " + result);
		}
		else
		{
			string body_bytes = NewTemp();
			Emit(body_bytes + " = binary sub i64 " + runtime_size +
			     ", " + to_string(header));
			runtime_count = NewTemp();
			Emit(runtime_count + " = binary udiv i64 " + body_bytes +
			     ", " + to_string(elem_size));
			Emit("store i64 " + runtime_count + ", " + result);
		}
	}
	if (ctor)
	{
		string bound;
		if (!runtime)
		{
			bound = NewTemp();
			Emit(bound + " = const i64 " + to_string(count.value.bits));
		}
		else
			bound = runtime_count;
		LowerNewArrayCtorLoop(*ctor, data, elem_size, bound);
	}
	if (node.trivial_init && !header)
	{
		string bound;
		if (!runtime)
		{
			bound = NewTemp();
			Emit(bound + " = const i64 " +
			     to_string(count.value.bits * elem_size));
		}
		else
			bound = runtime_size;
		LowerNewArrayZeroLoop(result, bound);
	}
	LowerValue value;
	value.type = RemoveTopCv(node.type);
	value.text = data;
	return value;
}

// Per-element default construction of an array new in index order.
void FunctionLowerer::LowerNewArrayCtorLoop(const SemNode& ctor,
                                            const string& data,
                                            unsigned long long elem_size,
                                            const string& bound)
{
	string index_slot = AddMatSlot("array_new_index", "i64");
	string cond_label = NewLabel("array_new_ctor_cond");
	string body_label = NewLabel("array_new_ctor_body");
	string end_label = NewLabel("array_new_ctor_end");
	Emit("store i64 0, $" + index_slot);
	CloseInto(cond_label);
	string at = NewTemp();
	Emit(at + " = load i64 $" + index_slot);
	string more = NewTemp();
	Emit(more + " = cmp ult i64 " + at + ", " + bound);
	ReferenceLabel(body_label);
	ReferenceLabel(end_label);
	Terminate("branch " + more + ", ^" + body_label + ", ^" +
	          end_label);
	OpenBlock(body_label);
	string offset = NewTemp();
	Emit(offset + " = binary mul i64 " + at + ", " +
	     to_string(elem_size));
	string element = NewTemp();
	Emit(element + " = index i8 " + data + ", " + offset);
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	LowerConstructorCall(ctor, element);
	in_lifetime_action_ = saved;
	string next = NewTemp();
	Emit(next + " = binary add i64 " + at + ", 1");
	Emit("store i64 " + next + ", $" + index_slot);
	ReferenceLabel(cond_label);
	Terminate("jump ^" + cond_label);
	OpenBlock(end_label);
}

// `()` value-initialization of an array new zero-fills the storage
// byte-wise.
void FunctionLowerer::LowerNewArrayZeroLoop(const string& result,
                                            const string& bound)
{
	string offset_slot = AddMatSlot("zeroinit_offset", "i64");
	string cond_label = NewLabel("zeroinit_cond");
	string body_label = NewLabel("zeroinit_body");
	string end_label = NewLabel("zeroinit_end");
	Emit("store i64 0, $" + offset_slot);
	CloseInto(cond_label);
	string at = NewTemp();
	Emit(at + " = load i64 $" + offset_slot);
	string more = NewTemp();
	Emit(more + " = cmp ult i64 " + at + ", " + bound);
	ReferenceLabel(body_label);
	ReferenceLabel(end_label);
	Terminate("branch " + more + ", ^" + body_label + ", ^" +
	          end_label);
	OpenBlock(body_label);
	string element = NewTemp();
	Emit(element + " = index i8 " + result + ", " + at);
	Emit("store i8 0, " + element);
	string next = NewTemp();
	Emit(next + " = binary add i64 " + at + ", 1");
	Emit("store i64 " + next + ", $" + offset_slot);
	ReferenceLabel(cond_label);
	Terminate("jump ^" + cond_label);
	OpenBlock(end_label);
}

// Scalar new of a non-class object: the allocation call plus the
// stored initializer value.
LowerValue FunctionLowerer::LowerNewInit(const SemNode& node)
{
	LowerValue address = LowerValueExpr(*node.children[0]);
	TypePtr element = RemoveTopCv(node.type->target);
	string init_label;
	string end_label;
	if (node.null_pointer && node.children.size() > 1)
	{
		// A non-throwing allocation: initialization runs only on a
		// non-null result.
		string check = NewTemp();
		Emit(check + " = cmp ne ptr " + address.text + ", 0");
		init_label = NewLabel("new_init");
		end_label = NewLabel("new_end");
		ReferenceLabel(init_label);
		ReferenceLabel(end_label);
		Terminate("branch " + check + ", ^" + init_label + ", ^" +
		          end_label);
		OpenBlock(init_label);
	}
	if (node.children.size() > 1)
	{
		string stored = LowerValueAs(*node.children[1], element,
		                             LCC_INIT);
		Emit("store " + LowerValueType(element) + " " + stored + ", " +
		     address.text);
	}
	if (!init_label.empty())
	{
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(end_label);
	}
	LowerValue value;
	value.type = RemoveTopCv(node.type);
	value.text = address.text;
	return value;
}

// PA17 5.3.5p3: a virtual destructor dispatches through the
// deleting-destructor slot, which also frees the storage.
void FunctionLowerer::LowerDeletingDispatch(const SemNode& dtor_callee,
                                            const string& pointer_text)
{
	string vpointer = NewTemp();
	Emit(vpointer + " = load ptr " + pointer_text);
	string slot_address = vpointer;
	if (dtor_callee.vtable_slot > 0)
	{
		slot_address = NewTemp();
		Emit(slot_address + " = index i8 " + vpointer + ", " +
		     to_string(8 * dtor_callee.vtable_slot));
	}
	string fn = NewTemp();
	Emit(fn + " = load ptr " + slot_address);
	Emit("call void " + fn + "(" + pointer_text + ")" +
	     IndirectCallSignature(dtor_callee.type));
}

// delete / delete[]: the null check, element destruction, and the
// deallocation call. Class arrays read their count header at -8.
void FunctionLowerer::LowerDelete(const SemNode& node)
{
	const SemNode& operand = *node.children[0];
	const SemNode& callee = *node.children[1];
	const SemNode* dtor = 0;
	for (size_t i = 2; i < node.children.size(); i++)
		if (node.children[i]->kind == SN_DESTRUCTOR_ACTION)
			dtor = node.children[i].get();
	bool array_form = node.kind == SN_DELETE_ARRAY;
	unsigned long long header = node.member_offset;
	unsigned long long elem_size = node.value.bits;
	LowerValue pointer = LowerValueExpr(operand);
	if (pointer.imm_null && pointer.text.empty())
		pointer.text = MaterializeNull();
	bool saved = in_lifetime_action_;
	in_lifetime_action_ = true;
	if (array_form && !header)
	{
		// A non-class array deallocates its pointer directly.
		string fn = program_.FunctionRef(callee.entity_scope,
		                                 callee.entity_name, callee.type);
		Emit("call void " + fn + "(" + pointer.text + ")");
		in_lifetime_action_ = saved;
		return;
	}
	string check = NewTemp();
	Emit(check + " = cmp ne ptr " + pointer.text + ", 0");
	string nonnull_label = NewLabel(array_form ? "array_delete_nonnull"
	                                           : "delete_nonnull");
	string end_label = NewLabel(array_form ? "array_delete_end"
	                                       : "delete_end");
	ReferenceLabel(nonnull_label);
	ReferenceLabel(end_label);
	Terminate("branch " + check + ", ^" + nonnull_label + ", ^" +
	          end_label);
	OpenBlock(nonnull_label);
	if (!array_form)
	{
		const SemNode* dtor_callee = dtor
			? dtor->children[0]->children[0].get() : 0;
		if (dtor_callee && dtor_callee->vtable_slot >= 0)
			LowerDeletingDispatch(*dtor_callee, pointer.text);
		else
		{
			if (dtor)
			{
				const SemNode& call = *dtor->children[0];
				Emit("call void " +
				     program_.MemberFunctionRef(*call.children[0]) + "(" +
				     pointer.text + ")");
			}
			string fn = program_.FunctionRef(callee.entity_scope,
			                                 callee.entity_name,
			                                 callee.type);
			Emit("call void " + fn + "(" + pointer.text + ")");
		}
	}
	else
	{
		string base = NewTemp();
		Emit(base + " = index i8 " + pointer.text + ", -" +
		     to_string(header));
		string stored_count = NewTemp();
		Emit(stored_count + " = load i64 " + base);
		if (dtor)
		{
			// Elements destroy in reverse construction order.
			string index_slot = AddMatSlot("array_delete_index", "i64");
			Emit("store i64 " + stored_count + ", $" + index_slot);
			string cond_label = NewLabel("array_delete_dtor_cond");
			string body_label = NewLabel("array_delete_dtor_body");
			string done_label = NewLabel("array_delete_dtor_end");
			CloseInto(cond_label);
			string at = NewTemp();
			Emit(at + " = load i64 $" + index_slot);
			string more = NewTemp();
			Emit(more + " = cmp ne i64 " + at + ", 0");
			ReferenceLabel(body_label);
			ReferenceLabel(done_label);
			Terminate("branch " + more + ", ^" + body_label + ", ^" +
			          done_label);
			OpenBlock(body_label);
			string previous = NewTemp();
			Emit(previous + " = binary sub i64 " + at + ", 1");
			Emit("store i64 " + previous + ", $" + index_slot);
			string offset = NewTemp();
			Emit(offset + " = binary mul i64 " + previous + ", " +
			     to_string(elem_size));
			string element = NewTemp();
			Emit(element + " = index i8 " + pointer.text + ", " + offset);
			const SemNode& call = *dtor->children[0];
			Emit("call void " +
			     program_.MemberFunctionRef(*call.children[0]) + "(" +
			     element + ")");
			ReferenceLabel(cond_label);
			Terminate("jump ^" + cond_label);
			OpenBlock(done_label);
		}
		string fn = program_.FunctionRef(callee.entity_scope,
		                                 callee.entity_name, callee.type);
		Emit("call void " + fn + "(" + base + ")");
	}
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(end_label);
	in_lifetime_action_ = saved;
}
