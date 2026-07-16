#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"

using std::runtime_error;
using std::to_string;

// PA25 exception handling and RTTI expressions: source-level
// throw/try/catch statement lowering (15.1/15.3) plus the typeid and
// dynamic_cast expression forms (5.2.7/5.2.8) that share the EH
// runtime scaffolding. The hidden unwind-region machinery lives in
// lower_member.cpp; the remaining statement and expression halves in
// lower_function.cpp and lower_expr.cpp.

namespace {

TypePtr StripReference(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

// The computation type of a node: declared type with references and
// top-level cv stripped.
TypePtr NodeType(const SemNode& node)
{
	return RemoveTopCv(StripReference(node.type));
}

TypePtr BoolType()
{
	return MakeFundamentalType(FT_BOOL);
}

}  // namespace

bool FunctionLowerer::IsTypeInfoComparison(const SemNode& node) const
{
	if (node.kind != SN_CALL_EXPRESSION || node.children.size() != 3 ||
	    node.children[0]->kind != SN_CALLEE)
		return false;
	const SemNode& callee = *node.children[0];
	return callee.is_method &&
		(callee.entity_name == "operator ==" ||
		 callee.entity_name == "operator !=") &&
		callee.entity_scope &&
		program_.IsStdTypeInfo(callee.entity_scope->entity);
}

// PA25 5.2.7: a polymorphic downcast dynamic_cast. The result slot
// pre-nulls, a null operand short-circuits, and the runtime
// __dynamic_cast query fills the slot; the reference form raises
// __cxa_bad_cast when the query fails.
// 5.2.8: std::type_info operator==/!= folds to RTTI record address
// identity - no runtime call.
LowerValue FunctionLowerer::LowerTypeInfoComparison(const SemNode& node,
                                                    const SemNode& callee)
{
	// The implicit object argument arrives as an address-of wrapper;
	// the reference parameter binds the lvalue directly.
	const SemNode& lhs_node =
		node.children[1]->kind == SN_UNARY_EXPRESSION &&
		node.children[1]->op == OP_AMP
			? *node.children[1]->children[0] : *node.children[1];
	const SemNode& rhs_node =
		node.children[2]->kind == SN_UNARY_EXPRESSION &&
		node.children[2]->op == OP_AMP
			? *node.children[2]->children[0] : *node.children[2];
	string lhs = LowerAddressExpr(lhs_node);
	string rhs = LowerAddressExpr(rhs_node);
	LowerValue result;
	result.type = BoolType();
	result.text = NewTemp();
	Emit(result.text + " = cmp " +
	     (callee.entity_name == "operator ==" ? "eq" : "ne") +
	     " ptr " + lhs + ", " + rhs);
	return result;
}

// The Itanium src2dst hint of a dynamic_cast: the static offset when
// the source is a unique public base of the target, -3 when it
// appears more than once, -1 across a virtual base, -2 when the
// classes are unrelated (the sibling scan) or the unique path is not
// public - the runtime shortcuts on a non-negative hint, so every
// edge must grant public access.
static long long DynamicCastHint(const NamedTypeInfo* target,
                                 const NamedTypeInfo* source)
{
	int hops = 0;
	unsigned long long offset = 0;
	EBasePath path = BaseSubobjectPath(target, source, hops, offset);
	if (path == BP_UNIQUE)
	{
		vector<ClassBaseEdge> edges;
		bool is_public = BaseAccessPath(target, source, edges);
		for (size_t i = 0; is_public && i < edges.size(); i++)
			is_public = edges[i].derived
				->direct_bases[edges[i].base_index].access == MA_PUBLIC;
		return is_public ? (long long)offset : -2;
	}
	if (path == BP_AMBIGUOUS)
		return -3;
	if (target->class_record &&
	    FindClassVBase(*target->class_record, source->class_record))
		return -1;
	return -2;
}

string FunctionLowerer::LowerDynamicCast(const SemNode& node)
{
	bool ref_form = node.category != VC_PRVALUE;
	string operand = ref_form
		? LowerAddressExpr(*node.children[0])
		: LowerValueExpr(*node.children[0]).text;
	TypePtr target = ref_form ? node.type
	                          : RemoveTopCv(node.type->target);
	string slot = AddMatSlot("dyn_cast", "ptr");
	string scan_label = NewLabel("dyn_cast_scan");
	string end_label = NewLabel("dyn_cast_end");
	Emit("store ptr 0, $" + slot);
	string is_null = NewTemp();
	Emit(is_null + " = cmp eq ptr " + operand + ", 0");
	ReferenceLabel(end_label);
	ReferenceLabel(scan_label);
	Terminate("branch " + is_null + ", ^" + end_label + ", ^" +
	          scan_label);
	if (!ref_form && IsVoidType(target))
	{
		// PA26 5.2.7p7: the void* form adjusts the operand to the most
		// derived object through the vtable's offset-to-top slot (the
		// i64 one slot before the RTTI pointer). The runtime-call form
		// stays beside it as the reference's dead presentation block.
		OpenBlock(scan_label);
		string vpointer = NewTemp();
		Emit(vpointer + " = load ptr " + operand);
		string top_slot = NewTemp();
		Emit(top_slot + " = index i8 " + vpointer + ", -16");
		string top_offset = NewTemp();
		Emit(top_offset + " = load i64 " + top_slot);
		string adjusted = NewTemp();
		Emit(adjusted + " = index i8 " + operand + ", " + top_offset);
		Emit("store ptr " + adjusted + ", $" + slot);
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(NewLabel("block"));
		string void_src = NewTemp();
		Emit(void_src + " = addr " +
		     program_.RttiTypeRef(node.typeid_operand));
		string void_dst = NewTemp();
		Emit(void_dst + " = addr " + program_.ExternalVoidRttiRef());
		string runtime = NewTemp();
		Emit(runtime + " = call ptr " +
		     program_.ExternalRuntimeFnRef(
				"__dynamic_cast",
				"(%arg0 : ptr, %arg1 : ptr, %arg2 : ptr, %arg3 : i64)"
				" -> ptr [linkage=c, binding=strong, "
				"object=__dynamic_cast]") +
		     "(" + operand + ", " + void_src + ", " + void_dst +
		     ", -2)");
		Emit("store ptr " + runtime + ", $" + slot);
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		OpenBlock(end_label);
		string result = NewTemp();
		Emit(result + " = load ptr $" + slot);
		return result;
	}
	OpenBlock(scan_label);
	string src_rtti = NewTemp();
	Emit(src_rtti + " = addr " +
	     program_.RttiTypeRef(node.typeid_operand));
	string dst_rtti = NewTemp();
	Emit(dst_rtti + " = addr " + program_.RttiTypeRef(target));
	// The queried class is dynamically used: its vtable (and virtual
	// members) anchor in the program image.
	if (target->kind == TK_CLASS && target->named->class_record)
		program_.VTableRef(target->named->class_record);
	long long hint = DynamicCastHint(RemoveTopCv(target)->named,
	                                 node.typeid_operand->named);
	string result = NewTemp();
	Emit(result + " = call ptr " +
	     program_.ExternalRuntimeFnRef(
			"__dynamic_cast",
			"(%arg0 : ptr, %arg1 : ptr, %arg2 : ptr, %arg3 : i64) -> "
			"ptr [linkage=c, binding=strong, object=__dynamic_cast]") +
	     "(" + operand + ", " + src_rtti + ", " + dst_rtti + ", " +
	     to_string(hint) + ")");
	Emit("store ptr " + result + ", $" + slot);
	if (!ref_form)
	{
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	else
	{
		string fail_label = NewLabel("dyn_cast_fail");
		string found_label = NewLabel("dyn_cast_found");
		string failed = NewTemp();
		Emit(failed + " = cmp eq ptr " + result + ", 0");
		ReferenceLabel(fail_label);
		ReferenceLabel(found_label);
		Terminate("branch " + failed + ", ^" + fail_label + ", ^" +
		          found_label);
		OpenBlock(fail_label);
		Emit("call void " +
		     program_.ExternalRuntimeFnRef(
				"__cxa_bad_cast",
				"() -> void [effects=readnone, unwind=may, "
				"return=noreturn, linkage=c, binding=strong, "
				"object=__cxa_bad_cast]") + "()");
		EmitZeroValueReturn();
		OpenBlock(found_label);
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
		// The reference presentation keeps a dead continuation block
		// between the found edge and the join.
		OpenBlock(NewLabel("block"));
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	OpenBlock(end_label);
	string loaded = NewTemp();
	Emit(loaded + " = load ptr $" + slot);
	return loaded;
}

// PA25 5.2.8: the address of the RTTI record a typeid query denotes.
// The static form addresses the operand type's record; the dynamic
// form null-checks the polymorphic glvalue, reads its vpointer, and
// loads the record pointer stored one slot before the first virtual
// entry (the vtable's RTTI head slot).
string FunctionLowerer::LowerTypeidAddress(const SemNode& node)
{
	if (!node.typeid_dynamic)
	{
		string result = NewTemp();
		Emit(result + " = addr " +
		     program_.RttiTypeRef(node.typeid_operand));
		return result;
	}
	string object = LowerAddressExpr(*node.children[0]);
	string is_null = NewTemp();
	Emit(is_null + " = cmp eq ptr " + object + ", 0");
	string fail_label = NewLabel("typeid_fail");
	string scan_label = NewLabel("typeid_scan");
	ReferenceLabel(fail_label);
	ReferenceLabel(scan_label);
	Terminate("branch " + is_null + ", ^" + fail_label + ", ^" +
	          scan_label);
	OpenBlock(fail_label);
	Emit("call void " +
	     program_.ExternalRuntimeFnRef(
			"__cxa_bad_typeid",
			"() -> void [effects=readnone, unwind=may, "
			"return=noreturn, linkage=c, binding=strong, "
			"object=__cxa_bad_typeid]") + "()");
	EmitZeroValueReturn();
	OpenBlock(scan_label);
	string vpointer = NewTemp();
	Emit(vpointer + " = load ptr " + object);
	string slot = NewTemp();
	Emit(slot + " = index i8 " + vpointer + ", -8");
	string record = NewTemp();
	Emit(record + " = load ptr " + slot);
	return record;
}

void FunctionLowerer::EmitZeroValueReturn()
{
	if (indirect_ret_ || IsVoidType(return_type_))
	{
		Terminate("return void");
		return;
	}
	TypePtr ret_bare = RemoveTopCv(return_type_);
	if (!IsReferenceType(return_type_) && ret_bare->kind == TK_CLASS)
	{
		if (retobj_slot_.empty())
			retobj_slot_ = AddMatSlot("retobj",
			                          LowerSlotType(ret_bare));
		Emit("zeroinit " + LowerObjSpan(ret_bare) + " $" +
		     retobj_slot_);
		Terminate("return " + LowerSlotType(ret_bare) + " $" +
		          retobj_slot_);
		return;
	}
	string type_text = LowerValueType(return_type_);
	if (LowerFloatType(return_type_))
		Terminate("return " + type_text + " " +
		          LowerFloatZero(return_type_));
	else if (type_text == "ptr")
	{
		string temp = NewTemp();
		Emit(temp + " = copy ptr nullptr");
		Terminate("return ptr " + temp);
	}
	else
		Terminate("return " + type_text + " 0");
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
	// The dispatch region closes once the branch value is secured;
	// pending condition temporaries destroy on per-edge cleanup
	// trampolines before the real targets.
	if (eh_open_)
		CloseEhRegion();
	size_t mark = fe_marks_.empty() ? 0 : fe_marks_.back();
	if (temp_cleanups_.size() > mark && !in_cleanup_emission_)
	{
		string true_cleanup = NewLabel("cond_true_cleanup");
		string false_cleanup = NewLabel("cond_false_cleanup");
		ReferenceLabel(true_cleanup);
		ReferenceLabel(false_cleanup);
		Terminate("branch " + truth.text + ", ^" + true_cleanup +
		          ", ^" + false_cleanup);
		OpenBlock(true_cleanup);
		EmitTempCleanups(mark);
		ReferenceLabel(true_label);
		Terminate("jump ^" + true_label);
		OpenBlock(false_cleanup);
		EmitTempCleanups(mark);
		ReferenceLabel(false_label);
		Terminate("jump ^" + false_label);
		temp_cleanups_.resize(mark);
		return;
	}
	ReferenceLabel(true_label);
	ReferenceLabel(false_label);
	Terminate("branch " + truth.text + ", ^" + true_label + ", ^" +
	          false_label);
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
		cond_arm_depth_++;
		OpenSegmentRegion(*node.children[1]);
		LowerEffect(*node.children[1]);
		if (eh_open_)
			CloseEhRegion();
		CloseInto(else_label);
		OpenSegmentRegion(*node.children[2]);
		LowerEffect(*node.children[2]);
		if (eh_open_)
			CloseEhRegion();
		cond_arm_depth_--;
		CloseInto(end_label);
		LowerValue value;
		value.type = NodeType(node);
		return value;
	}
	TypePtr type = NodeType(node);
	string slot = AddMatSlot("cond", LowerValueType(type));
	BranchOnValue(*node.children[0], then_label, else_label);
	OpenBlock(then_label);
	cond_arm_depth_++;
	OpenSegmentRegion(*node.children[1]);
	string first = LowerValueAs(*node.children[1], type, LCC_INIT);
	Emit("store " + LowerValueType(type) + " " + first + ", $" + slot);
	if (eh_open_)
		CloseEhRegion();
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(else_label);
	OpenSegmentRegion(*node.children[2]);
	string second = LowerValueAs(*node.children[2], type, LCC_INIT);
	Emit("store " + LowerValueType(type) + " " + second + ", $" + slot);
	if (eh_open_)
		CloseEhRegion();
	cond_arm_depth_--;
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
		cond_arm_depth_++;
		OpenSegmentRegion(operand);
		string address = LowerAddressExpr(operand);
		TypePtr source = NodeType(operand);
		// A derived arm adjusts to the common base result (5.16p3).
		if (source->kind == TK_CLASS && result_type->kind == TK_CLASS)
		{
			address = AdjustToBase(address, source->named,
			                       result_type->named);
		}
		Emit("store ptr " + address + ", $" + slot);
		if (eh_open_)
			CloseEhRegion();
		cond_arm_depth_--;
		ReferenceLabel(end_label);
		Terminate("jump ^" + end_label);
	}
	OpenBlock(end_label);
	string result = NewTemp();
	Emit(result + " = load ptr $" + slot);
	return result;
}

// Emits one eh_catch/eh_catch_all marker per handler, assigning the
// function-global selector ids on first emission.
void FunctionLowerer::EmitTryMarkers(EhContext& context)
{
	for (size_t i = 0; i < context.handlers.size(); i++)
	{
		EhHandler& handler = context.handlers[i];
		if (!handler.selector)
			handler.selector = ++catch_selector_counter_;
		if (handler.catch_all)
			Emit("eh_catch_all, " + to_string(handler.selector));
		else
			Emit("eh_catch " + handler.rtti + ", " +
			     to_string(handler.selector));
	}
}

// A return crossing active try/catch contexts pops their markers: the
// catch contexts close (and end the caught exception) before the
// scope cleanups run, the try contexts after.
void FunctionLowerer::EmitEhReturnUnwind()
{
	for (size_t i = eh_contexts_.size(); i-- > 0;)
		if (eh_contexts_[i].is_catch)
		{
			Emit("eh_end");
			Emit("call void " +
			     program_.ExternalRuntimeFnRef(
					"__cxa_end_catch",
					"() -> void [role=eh_end_catch, linkage=c, "
					"binding=strong, object=__cxa_end_catch]") + "()");
		}
	EmitCleanupsFrom(0);
	for (size_t i = eh_contexts_.size(); i-- > 0;)
		if (!eh_contexts_[i].is_catch)
			Emit("eh_end");
}

void FunctionLowerer::LowerTry(const SemNode& node)
{
	program_.RequireEhRuntime();
	EhContext context;
	context.dispatch_label = NewLabel("catch_dispatch");
	context.entry_label = NewLabel("catch_entry");
	context.end_label = NewLabel("try_end");
	context.cleanup_depth = cleanup_scopes_.size();
	context.ctor_rethrow = node.function_try;
	// A constructor function-try-block leads with the member/base
	// initialization actions; the handlers follow the body.
	size_t first_handler = 1;
	if (node.function_try)
	{
		first_handler = 0;
		while (first_handler < node.children.size() &&
		       node.children[first_handler]->kind != SN_CATCH_HANDLER)
			first_handler++;
	}
	for (size_t i = first_handler; i < node.children.size(); i++)
	{
		const SemNode& handler_node = *node.children[i];
		EhHandler handler;
		handler.node = &handler_node;
		if (handler_node.type)
			handler.rtti = program_.ThrowRttiRef(
				RemoveTopCv(StripReference(handler_node.type)));
		else
			handler.catch_all = true;
		context.handlers.push_back(handler);
	}
	ReferenceLabel(context.dispatch_label);
	Emit("eh_try ^" + context.dispatch_label);
	eh_contexts_.push_back(context);
	for (size_t i = 0; i < first_handler; i++)
		LowerStatement(*node.children[i]);
	context = eh_contexts_.back();
	eh_contexts_.pop_back();
	if (!blocks_.back().terminated)
	{
		Emit("eh_end");
		ReferenceLabel(context.end_label);
		Terminate("jump ^" + context.end_label);
	}
	// The landing dispatch: the handler markers, then the shared
	// selector comparison chain.
	OpenBlock(context.dispatch_label);
	EmitTryMarkers(context);
	ReferenceLabel(context.entry_label);
	Terminate("jump ^" + context.entry_label);
	OpenBlock(context.entry_label);
	string exc = NewTemp();
	Emit(exc + " = exception ptr");
	string selector = NewTemp();
	Emit(selector + " = exception_selector i32");
	for (size_t i = 0; i < context.handlers.size(); i++)
		context.handlers[i].body_label = NewLabel("catch_body");
	context.next_label = NewLabel("catch_next");
	for (size_t i = 0; i < context.handlers.size(); i++)
	{
		EhHandler& handler = context.handlers[i];
		string miss = i + 1 < context.handlers.size()
			? NewLabel("catch_check") : context.next_label;
		string matched = NewTemp();
		Emit(matched + " = cmp eq i32 " + selector + ", " +
		     to_string(handler.selector));
		ReferenceLabel(handler.body_label);
		ReferenceLabel(miss);
		Terminate("branch " + matched + ", ^" + handler.body_label +
		          ", ^" + miss);
		if (i + 1 < context.handlers.size())
			OpenBlock(miss);
	}
	// The fn-try handlers run after every armed subobject was already
	// destroyed on the way in (15.2p2); only the unmatched-selector
	// path still owns their destruction.
	vector<const SemNode*> saved_ctor_cleanups;
	if (node.function_try)
		saved_ctor_cleanups.swap(ctor_cleanups_);
	for (size_t i = 0; i < context.handlers.size(); i++)
		EmitCatchHandler(context, context.handlers[i], exc);
	if (node.function_try)
		ctor_cleanups_.swap(saved_ctor_cleanups);
	EmitCatchNext(context);
	// The join survives even when every path terminated (the
	// reference keeps the dead continuation).
	ReferenceLabel(context.end_label);
	OpenBlock(context.end_label);
}

// The by-value handler parameter's destruction (before end_catch).
void FunctionLowerer::EmitCatchParamDtor(const string& slot,
                                         const string& dtor)
{
	if (dtor.empty())
		return;
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	Emit("call void " + dtor + "(" + address + ")");
}

void FunctionLowerer::EmitCatchParamInit(const SemNode& handler_node,
                                         const string& object,
                                         string& param_slot,
                                         string& param_dtor)
{
	if (!handler_node.entity_name.empty())
	{
		TypePtr declared = handler_node.type;
		if (IsReferenceType(declared))
		{
			string slot = AddSlot(handler_node.entity_scope,
			                      handler_node.entity_name, "ptr");
			Emit("store ptr " + object + ", $" + slot);
		}
		else
		{
			TypePtr bare = RemoveTopCv(declared);
			if (bare->kind != TK_CLASS)
			{
				string slot = AddSlot(handler_node.entity_scope,
				                      handler_node.entity_name,
				                      LowerSlotType(bare));
				string value = NewTemp();
				Emit(value + " = load " + LowerValueType(bare) +
				     " " + object);
				Emit("store " + LowerValueType(bare) + " " + value +
				     ", $" + slot);
			}
		}
	}
	// A by-value class handler copy-initializes its parameter object
	// (named or not) from the exception object; the object destroys
	// on every handler exit before end_catch.
	if (handler_node.handler_ctor && handler_node.type &&
	    !IsReferenceType(handler_node.type))
	{
		TypePtr bare = RemoveTopCv(handler_node.type);
		param_slot = handler_node.entity_name.empty()
			? AddMatSlot("__catch", LowerSlotType(bare))
			: AddSlot(handler_node.entity_scope,
			          handler_node.entity_name,
			          LowerSlotType(bare));
		string address = NewTemp();
		Emit(address + " = addr $" + param_slot);
		string ctor = program_.MemberFunctionRef(
			*handler_node.handler_ctor->children[0]->children[0]);
		Emit("call void " + ctor + "(" + address + ", " + object +
		     ")");
		if (handler_node.subobject_dtor)
			param_dtor = program_.MemberFunctionRef(
				*handler_node.subobject_dtor
					->children[0]->children[0]);
	}
}

// One catch handler: __cxa_begin_catch, the declared entity binding,
// the guarded body, and the handler's own unwind path (ending the
// caught exception before continuing outward).
void FunctionLowerer::EmitCatchHandler(EhContext& context,
                                       EhHandler& handler,
                                       const string& exc)
{
	string end_catch_ref = program_.ExternalRuntimeFnRef(
		"__cxa_end_catch",
		"() -> void [role=eh_end_catch, linkage=c, binding=strong, "
		"object=__cxa_end_catch]");
	{
		const SemNode& handler_node = *handler.node;
		OpenBlock(handler.body_label);
		string object = NewTemp();
		Emit(object + " = call ptr " +
		     program_.ExternalRuntimeFnRef(
				"__cxa_begin_catch",
				"(%arg0 : ptr) -> ptr [role=eh_begin_catch, linkage=c, "
				"binding=strong, object=__cxa_begin_catch]") +
		     "(" + exc + ")");
		if (!handler_node.entity_name.empty())
		{
			string slot = AddMatSlot("catch", "ptr");
			Emit("store ptr " + object + ", $" + slot);
		}
		string cleanup_label = NewLabel("catch_cleanup");
		ReferenceLabel(cleanup_label);
		Emit("eh_cleanup ^" + cleanup_label);
		string param_slot;
		string param_dtor;
		EmitCatchParamInit(handler_node, object, param_slot,
		                   param_dtor);
		EhContext catch_context;
		catch_context.is_catch = true;
		catch_context.cleanup_label = cleanup_label;
		catch_context.cleanup_depth = cleanup_scopes_.size();
		eh_contexts_.push_back(catch_context);
		LowerStatement(*handler_node.children[0]);
		eh_contexts_.pop_back();
		if (!blocks_.back().terminated)
		{
			if (context.ctor_rethrow)
			{
				// 15.3p15: flowing off a constructor's handler end
				// rethrows; the armed catch cleanup ends the caught
				// exception as the rethrow unwinds through it.
				EmitCatchParamDtor(param_slot, param_dtor);
				Emit("call void " +
				     program_.ExternalRuntimeFnRef(
						"__cxa_rethrow",
						"() -> void [return=noreturn, role=eh_rethrow, "
						"linkage=c, binding=strong, "
						"object=__cxa_rethrow]") +
				     "()");
				Terminate("return void");
			}
			else
			{
				Emit("eh_end");
				EmitCatchParamDtor(param_slot, param_dtor);
				Emit("call void " + end_catch_ref + "()");
				ReferenceLabel(context.end_label);
				Terminate("jump ^" + context.end_label);
			}
		}
		// The unwind path out of the handler ends the caught
		// exception, pops the try marker, and continues.
		OpenBlock(cleanup_label);
		size_t enclosing = eh_contexts_.size();
		while (enclosing > 0 && eh_contexts_[enclosing - 1].is_catch)
			enclosing--;
		if (enclosing > 0)
		{
			EhContext& outer = eh_contexts_[enclosing - 1];
			EmitTryMarkers(outer);
			EmitCatchParamDtor(param_slot, param_dtor);
			Emit("call void " + end_catch_ref + "()");
			Emit("eh_end");
			Emit("eh_end");
			ReferenceLabel(outer.entry_label);
			Terminate("jump ^" + outer.entry_label);
		}
		else
		{
			EmitCatchParamDtor(param_slot, param_dtor);
			Emit("call void " + end_catch_ref + "()");
			Emit("eh_end");
			EmitCtorUnwindCleanups();
			Terminate("resume");
		}
	}
}

// The selector missed every handler of this try: continue to the
// enclosing in-function try (running the scopes between) or resume
// unwinding.
void FunctionLowerer::EmitCatchNext(EhContext& context)
{
	OpenBlock(context.next_label);
	size_t enclosing = eh_contexts_.size();
	while (enclosing > 0 && eh_contexts_[enclosing - 1].is_catch)
		enclosing--;
	if (enclosing > 0)
	{
		EhContext& outer = eh_contexts_[enclosing - 1];
		EmitCleanupsFrom(outer.cleanup_depth);
		ReferenceLabel(outer.entry_label);
		Terminate("jump ^" + outer.entry_label);
	}
	else
	{
		EmitCtorUnwindCleanups();
		Terminate("resume");
	}
}

// PA25 15.1: throw allocates the exception object, builds the payload
// (a stored scalar or a copy-construction), and raises it through
// __cxa_throw; a rethrow re-raises the caught exception. Both are
// noreturn - a zero-valued return terminates the dead continuation.
void FunctionLowerer::LowerThrow(const SemNode& node)
{
	program_.RequireEhRuntime();
	if (node.children.empty())
	{
		Emit("call void " +
		     program_.ExternalRuntimeFnRef(
				"__cxa_rethrow",
				"() -> void [return=noreturn, role=eh_rethrow, "
				"linkage=c, binding=strong, object=__cxa_rethrow]") +
		     "()");
		EmitZeroValueReturn();
		return;
	}
	TypePtr type = node.typeid_operand;
	bool is_class = type->kind == TK_CLASS;
	program_.EhObjGlobal(type);
	string alloc_ref = program_.ExternalRuntimeFnRef(
		"__cxa_allocate_exception",
		"(%arg0 : i64) -> ptr [role=eh_allocate_exception, linkage=c, "
		"binding=strong, object=__cxa_allocate_exception]");
	string throw_ref = program_.ExternalRuntimeFnRef(
		"__cxa_throw",
		"(%arg0 : ptr, %arg1 : ptr, %arg2 : ptr) -> void "
		"[return=noreturn, role=eh_throw, linkage=c, binding=strong, "
		"object=__cxa_throw]");
	// Destructible locals and armed constructor subobjects must
	// destroy when the raise leaves the function (15.2p2), so their
	// cleanups also put the throw under a dispatch region.
	bool wrap = !in_cleanup_emission_ && !suppress_eh_regions_ &&
		(eh_armed_ || !temp_cleanups_.empty() ||
		 !ctor_cleanups_.empty());
	if (wrap && !eh_open_)
		OpenEhRegion("throw_alloc_unwind_end");
	string storage = NewTemp();
	Emit(storage + " = call ptr " + alloc_ref + "(" +
	     to_string(TypeSize(type)) + ")");
	if (is_class)
	{
		// The storage pointer survives the allocation's dispatch
		// region in its slot; the payload copy-constructs into it.
		string slot = AddMatSlot("throw_alloc", "ptr");
		Emit("store ptr " + storage + ", $" + slot);
		if (eh_open_)
			CloseEhRegion();
		storage = NewTemp();
		Emit(storage + " = load ptr $" + slot);
		// 12.8p31: a prvalue thrown operand constructs the exception
		// object directly - the selected copy elides (the reference
		// shape), though it stays odr-used.
		const SemNode* init = node.children[0].get();
		if (init->kind == SN_CONSTRUCTOR_ACTION &&
		    !init->children.empty() &&
		    init->children[0]->children.size() == 2)
		{
			const SemNode& source = *init->children[0]->children[1];
			if (source.category == VC_PRVALUE && source.type &&
			    source.kind == SN_CONSTRUCTOR_ACTION &&
			    TypeEquals(RemoveTopCv(source.type),
			               RemoveTopCv(type)))
			{
				program_.DemandElidedCtor(
					*init->children[0]->children[0]);
				init = &source;
			}
		}
		LowerClassInit(*init, storage);
		if (wrap && !eh_open_)
			OpenEhRegion();
	}
	else
	{
		string value = LowerValueAs(*node.children[0],
		                            RemoveTopCv(type), LCC_INIT);
		Emit("store " + LowerValueType(type) + " " + value + ", " +
		     storage);
	}
	string rtti = NewTemp();
	Emit(rtti + " = addr " + program_.ThrowRttiRef(type));
	string dtor_arg = "0";
	if (is_class && node.children.size() > 1)
	{
		const SemNode& dtor_call = *node.children[1]->children[0];
		string target =
			program_.MemberFunctionRef(*dtor_call.children[0]);
		dtor_arg = NewTemp();
		Emit(dtor_arg + " = addr " + target);
	}
	Emit("call void " + throw_ref + "(" + storage + ", " + rtti + ", " +
	     dtor_arg + ")");
	if (eh_open_)
		CloseEhRegion();
	if (!eh_contexts_.empty() && !eh_contexts_.back().is_catch)
		Emit("eh_end");
	EmitZeroValueReturn();
}
