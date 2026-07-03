#include "sema/const_eval.h"

#include <stdexcept>

using std::runtime_error;

// Calls, constructors, and statement execution of the PA20 constant
// engine.

namespace {

const int kDepthLimit = 512;

runtime_error NotConstant(const string& what)
{
	return runtime_error(what + " is not a constant expression");
}

TypePtr ValueTypeOf(const SemNode& node)
{
	TypePtr type = node.type;
	if (type && IsReferenceType(type))
		type = type->target;
	return type;
}

// RAII frame push/pop so evaluation errors unwind cleanly.
struct FramePusher
{
	FramePusher(vector<ConstEvalEngine::Frame*>& frames,
	            ConstEvalEngine::Frame& frame, int& depth)
		: frames_(frames), depth_(depth)
	{
		if (++depth_ > kDepthLimit)
		{
			--depth_;
			throw NotConstant("call nesting beyond the depth limit");
		}
		frames_.push_back(&frame);
	}
	~FramePusher()
	{
		frames_.pop_back();
		--depth_;
	}

private:
	vector<ConstEvalEngine::Frame*>& frames_;
	int& depth_;
};

}  // namespace

// --- initializer application -------------------------------------------------

void ConstEvalEngine::EvalBracedInit(const SemNode& node,
                                     const ConstPointer& dest,
                                     const TypePtr& type)
{
	TypePtr bare = RemoveTopCv(type);
	if (bare->kind == TK_ARRAY)
	{
		TypePtr element = bare->target;
		unsigned long long size = TypeSize(RemoveTopCv(element));
		for (size_t i = 0; i < node.children.size(); i++)
		{
			ConstPointer at = dest;
			at.offset += i * size;
			EvalInit(*node.children[i], at, element);
		}
		return;
	}
	if (bare->kind == TK_CLASS)
	{
		const ClassInfo* cls = unit_.classes.Find(bare->named);
		if (!cls || cls->base)
			throw NotConstant("braced class initializer");
		size_t item = 0;
		for (size_t f = 0; f < cls->fields.size() &&
		     item < node.children.size(); f++)
		{
			const ClassField& field = cls->fields[f];
			if (field.name.empty())
				continue;
			ConstPointer at = dest;
			at.offset += field.offset;
			EvalInit(*node.children[item++], at, field.type);
		}
		return;
	}
	// A one-element braced scalar initializer.
	if (node.children.size() == 1)
	{
		EvalInit(*node.children[0], dest, bare);
		return;
	}
	throw NotConstant("braced initializer form");
}

void ConstEvalEngine::ApplyInitAction(const SemNode& child,
                                      const ConstPointer& dest,
                                      const TypePtr& type)
{
	switch (child.kind)
	{
	case SN_DESTRUCTOR_ACTION:
		return;
	case SN_EXPRESSION_STATEMENT:
		// A member-wise aggregate store targets the object by name.
		EvalEffect(*child.children[0]);
		return;
	case SN_ASSIGNMENT_EXPRESSION:
		EvalAssignment(child);
		return;
	case SN_STORAGE_COPY:
		CopyBytes(EvalLValue(*child.children[0]),
		          EvalLValue(*child.children[1]), child.value.bits);
		return;
	default:
		EvalInit(child, dest, type);
	}
}

void ConstEvalEngine::EvalInit(const SemNode& node,
                               const ConstPointer& dest,
                               const TypePtr& type)
{
	Step();
	TypePtr bare = RemoveTopCv(type);
	switch (node.kind)
	{
	case SN_CONSTRUCTOR_ACTION:
		EvalConstructorAction(node, &dest, bare);
		return;
	case SN_BRACED_INIT_LIST:
		EvalBracedInit(node, dest, bare);
		return;
	case SN_STORAGE_COPY:
		CopyBytes(EvalLValue(*node.children[0]),
		          EvalLValue(*node.children[1]), node.value.bits);
		return;
	case SN_CALL_EXPRESSION:
		if (bare->kind == TK_CLASS && node.category == VC_PRVALUE)
		{
			EvalCall(node, &dest);
			return;
		}
		break;
	case SN_CONDITIONAL_EXPRESSION:
		if (bare->kind == TK_CLASS || bare->kind == TK_ARRAY)
		{
			EvalInit(*node.children[EvaluateBool(*node.children[0])
			                        ? 1 : 2],
			         dest, bare);
			return;
		}
		break;
	default:
		break;
	}
	if (bare->kind == TK_CLASS)
	{
		// Copy from an existing constant object (trivial copies only
		// reach here; constructor forms carry explicit actions).
		CopyBytes(dest, EvalLValue(node), TypeSize(bare));
		return;
	}
	if (bare->kind == TK_ARRAY)
		throw NotConstant("array initializer form");
	WriteValue(dest, bare, Convert(EvalRead(node), bare));
}

// --- constructor evaluation ---------------------------------------------------

void ConstEvalEngine::EvalConstructorAction(const SemNode& node,
                                            const ConstPointer* dest,
                                            const TypePtr& dest_type)
{
	Step();
	if (node.elided || node.trivial_init || node.children.empty())
		return;  // zero-initialized images already hold the result
	const SemNode& call = *node.children[0];
	if (call.kind != SN_CALL_EXPRESSION || call.children.empty())
		throw NotConstant("constructor action form");
	const SemNode& callee = *call.children[0];
	size_t args_at = 1;
	ConstPointer target;
	if (node.ctor_addressed)
	{
		EvalValue address = EvalRead(*call.children[1]);
		if (address.kind != EvalValue::EV_PTR || address.ptr.IsNull())
			throw NotConstant("constructor target");
		target = address.ptr;
		args_at = 2;
	}
	else if (dest)
		target = *dest;
	else
	{
		TypePtr type = dest_type ? dest_type
		                         : RemoveTopCv(ValueTypeOf(node));
		ConstObjectPtr object = Allocate(type);
		target.object = object;
	}
	if (node.trivial_copy)
	{
		// The trivial copy/move constructor: a raw object copy of the
		// source (or a direct build when the source is a prvalue).
		if (call.children.size() <= args_at)
			throw NotConstant("trivial copy source");
		const SemNode& source = *call.children[args_at];
		TypePtr type = RemoveTopCv(ValueTypeOf(source));
		if (source.category == VC_PRVALUE &&
		    (source.kind == SN_CONSTRUCTOR_ACTION ||
		     source.kind == SN_CALL_EXPRESSION ||
		     source.kind == SN_BRACED_INIT_LIST))
			EvalInit(source, target, type);
		else
			CopyBytes(target, EvalLValue(source), TypeSize(type));
		return;
	}
	const SemNode* definition = registry_.Find(callee);
	if (!definition)
		throw NotConstant("call to an undefined constructor");
	if (!definition->is_constexpr_fn && !definition->synthesized)
		throw NotConstant("call to a non-constexpr constructor");
	RunConstructor(call, *definition, target, args_at);
}

void ConstEvalEngine::RunConstructor(const SemNode& call,
                                     const SemNode& definition,
                                     const ConstPointer& object,
                                     size_t args_at)
{
	Frame frame;
	frame.ret_type = MakeFundamentalType(FT_VOID);
	size_t params = 0;
	while (params < definition.children.size() &&
	       definition.children[params]->kind == SN_PARAMETER)
		params++;
	if (params == 0)
		throw NotConstant("constructor definition form");
	const SemNode& this_param = *definition.children[0];
	frame.slots[pair<const void*, string>(this_param.entity_scope,
	                                      this_param.entity_name)] =
		object;
	size_t declared = params - 1;
	if (call.children.size() - args_at != declared)
		throw NotConstant("constructor argument count");
	// Arguments evaluate in the calling frame; the constructor frame
	// activates only for the body run.
	for (size_t i = 0; i < declared; i++)
		BindOneArgument(*call.children[args_at + i],
		                *definition.children[1 + i], frame);
	FramePusher pushed(frames_, frame, depth_);
	RunBody(definition, frame);
}

// --- calls --------------------------------------------------------------------

void ConstEvalEngine::BindOneArgument(const SemNode& arg,
                                      const SemNode& param, Frame& frame)
{
	pair<const void*, string> key(param.entity_scope, param.entity_name);
	TypePtr declared = param.type;
	if (IsReferenceType(declared))
	{
		TypePtr referee = RemoveTopCv(declared->target);
		if (arg.category == VC_PRVALUE && arg.kind != SN_LITERAL)
		{
			ConstObjectPtr temp = Allocate(referee);
			ConstPointer address;
			address.object = temp;
			EvalInit(arg, address, referee);
			frame.slots[key] = address;
			return;
		}
		frame.slots[key] = EvalLValue(arg);
		return;
	}
	TypePtr bare = RemoveTopCv(declared);
	ConstObjectPtr storage = Allocate(bare);
	ConstPointer address;
	address.object = storage;
	if (bare->kind == TK_CLASS)
		EvalInit(arg, address, bare);
	else
		WriteScalar(*storage, 0, bare, Convert(EvalRead(arg), bare));
	frame.slots[key] = address;
}

EvalValue ConstEvalEngine::EvalCall(const SemNode& node,
                                    const ConstPointer* result_dest)
{
	Step();
	if (node.children.empty() || node.children[0]->kind != SN_CALLEE)
		throw NotConstant("indirect call");
	const SemNode& callee = *node.children[0];
	if (callee.vtable_slot >= 0)
		throw NotConstant("virtual dispatch");
	if (callee.special == SF_DESTRUCTOR ||
	    callee.special == SF_DESTRUCTOR_BASE)
		return EvalValue();  // trivial destruction of literal objects
	if (callee.special == SF_CONSTRUCTOR ||
	    callee.special == SF_CONSTRUCTOR_BASE)
	{
		// A bare constructor call (member/base initialization outside
		// an action wrapper): children[1] addresses the object.
		const SemNode* definition = registry_.Find(callee);
		if (!definition)
			throw NotConstant("call to an undefined constructor");
		if (!definition->is_constexpr_fn && !definition->synthesized)
			throw NotConstant("call to a non-constexpr constructor");
		EvalValue address = EvalRead(*node.children[1]);
		if (address.kind != EvalValue::EV_PTR || address.ptr.IsNull())
			throw NotConstant("constructor target");
		RunConstructor(node, *definition, address.ptr, 2);
		return EvalValue();
	}
	const SemNode* definition = registry_.Find(callee);
	if (!definition)
		throw NotConstant("call to an undefined function");
	if (!definition->is_constexpr_fn && !definition->synthesized)
		throw NotConstant("call to a non-constexpr function");
	Frame frame;
	TypePtr fn_type = callee.type;
	if (fn_type->kind != TK_FUNCTION)
		throw NotConstant("callee type");
	frame.ret_type = fn_type->target;
	TypePtr ret_bare = IsReferenceType(frame.ret_type)
		? TypePtr() : RemoveTopCv(frame.ret_type);
	if (ret_bare && ret_bare->kind == TK_CLASS)
	{
		if (result_dest)
			frame.ret_target = *result_dest;
		else
		{
			ConstObjectPtr discard = Allocate(ret_bare);
			frame.ret_target.object = discard;
		}
		frame.ret_target_valid = true;
	}
	// Arguments evaluate in the calling frame.
	BindArguments(node, *definition, frame);
	FramePusher pushed(frames_, frame, depth_);
	RunBody(*definition, frame);
	if (!frame.returned)
	{
		if (!IsVoidType(frame.ret_type))
			throw NotConstant("flowing off the end of a value return");
		return EvalValue();
	}
	return frame.ret_value;
}

void ConstEvalEngine::BindArguments(const SemNode& call,
                                    const SemNode& definition,
                                    Frame& frame)
{
	size_t params = 0;
	while (params < definition.children.size() &&
	       definition.children[params]->kind == SN_PARAMETER)
		params++;
	if (call.children.size() - 1 != params)
		throw NotConstant("argument count");
	for (size_t i = 0; i < params; i++)
	{
		const SemNode& param = *definition.children[i];
		const SemNode& arg = *call.children[1 + i];
		if (param.entity_name == "this")
		{
			EvalValue object = EvalRead(arg);
			if (object.kind != EvalValue::EV_PTR ||
			    object.ptr.IsNull())
				throw NotConstant("object argument");
			frame.slots[pair<const void*, string>(
				param.entity_scope, param.entity_name)] = object.ptr;
			continue;
		}
		BindOneArgument(arg, param, frame);
	}
}

// --- statements ---------------------------------------------------------------

void ConstEvalEngine::RunBody(const SemNode& definition, Frame& frame)
{
	for (size_t i = 0; i < definition.children.size(); i++)
	{
		const SemNode& child = *definition.children[i];
		if (child.kind == SN_PARAMETER)
			continue;
		if (ExecStatement(child, frame) == FLOW_RETURN)
			return;
	}
}

void ConstEvalEngine::ExecLocalVariable(const SemNode& node, Frame& frame)
{
	if (node.is_static_decl || node.is_extern_decl ||
	    node.is_thread_local_decl)
		throw NotConstant("non-automatic local in constant evaluation");
	pair<const void*, string> key(node.entity_scope, node.entity_name);
	if (IsReferenceType(node.type))
	{
		if (node.children.empty())
			throw NotConstant("reference without an initializer");
		frame.slots[key] = EvalLValue(*node.children[0]);
		return;
	}
	TypePtr bare = RemoveTopCv(node.type);
	ConstObjectPtr object = Allocate(bare);
	ConstPointer address;
	address.object = object;
	frame.slots[key] = address;
	for (size_t i = 0; i < node.children.size(); i++)
		ApplyInitAction(*node.children[i], address, bare);
}

bool ConstEvalEngine::EvalConditionNode(const SemNode& node, Frame& frame)
{
	const SemNode* inner = &node;
	if (inner->kind == SN_CONDITION)
		inner = inner->children[0].get();
	if (inner->kind == SN_CONDITION_DECLARATION)
	{
		// The declared variable initializes each evaluation; a scalar
		// declaration is itself the condition value (6.4p4), a class
		// declaration carries the converted value as the last child.
		const SemNode* declared = 0;
		for (size_t i = 0; i < inner->children.size(); i++)
			if (inner->children[i]->kind == SN_VARIABLE)
			{
				declared = inner->children[i].get();
				ExecLocalVariable(*declared, frame);
			}
		const SemNode& last = *inner->children.back();
		if (last.kind != SN_VARIABLE)
			return EvaluateBool(last);
		if (!declared)
			throw runtime_error("condition declaration form is not a "
			                    "constant expression");
		pair<const void*, string> key(declared->entity_scope,
		                              declared->entity_name);
		map<pair<const void*, string>, ConstPointer>::const_iterator
			slot = frame.slots.find(key);
		if (slot == frame.slots.end())
			throw runtime_error("condition variable is not a constant "
			                    "expression");
		EvalValue value = ReadThrough(slot->second,
		                              RemoveTopCv(declared->type));
		return value.kind == EvalValue::EV_INT
			? value.ival.bits != 0
			: value.kind == EvalValue::EV_FLOAT
				? value.fval != 0 : !value.ptr.IsNull();
	}
	return EvaluateBool(*inner);
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecCompound(const SemNode& node,
                                                     Frame& frame)
{
	for (size_t i = 0; i < node.children.size(); i++)
	{
		EFlow flow = ExecStatement(*node.children[i], frame);
		if (flow != FLOW_NORMAL)
			return flow;
	}
	return FLOW_NORMAL;
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecIf(const SemNode& node,
                                               Frame& frame)
{
	bool truth = EvalConditionNode(*node.children[0], frame);
	for (size_t i = 1; i < node.children.size(); i++)
	{
		const SemNode& arm = *node.children[i];
		if ((truth && arm.kind == SN_THEN) ||
		    (!truth && arm.kind == SN_ELSE))
			return arm.children.empty()
				? FLOW_NORMAL : ExecStatement(*arm.children[0], frame);
	}
	return FLOW_NORMAL;
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecLoop(const SemNode& node,
                                                 Frame& frame)
{
	bool is_do = node.kind == SN_DO_STATEMENT;
	const SemNode& condition = *node.children[is_do ? 1 : 0];
	const SemNode& body = *node.children[is_do ? 0 : 1];
	for (;;)
	{
		Step();
		if (!is_do && !EvalConditionNode(condition, frame))
			return FLOW_NORMAL;
		EFlow flow = ExecStatement(body, frame);
		if (flow == FLOW_RETURN)
			return flow;
		if (flow == FLOW_BREAK)
			return FLOW_NORMAL;
		if (is_do && !EvalConditionNode(condition, frame))
			return FLOW_NORMAL;
	}
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecFor(const SemNode& node,
                                                Frame& frame)
{
	const SemNode* condition = 0;
	const SemNode* iteration = 0;
	const SemNode* body = 0;
	for (size_t i = 0; i < node.children.size(); i++)
	{
		const SemNode& child = *node.children[i];
		if (child.kind == SN_FOR_INIT)
		{
			for (size_t j = 0; j < child.children.size(); j++)
				if (ExecStatement(*child.children[j], frame) !=
				    FLOW_NORMAL)
					throw NotConstant("for-init control flow");
		}
		else if (child.kind == SN_CONDITION)
			condition = &child;
		else if (child.kind == SN_ITERATION)
			iteration = &child;
		else
			body = &child;
	}
	if (!body)
		throw NotConstant("for statement form");
	for (;;)
	{
		Step();
		if (condition && !EvalConditionNode(*condition, frame))
			return FLOW_NORMAL;
		EFlow flow = ExecStatement(*body, frame);
		if (flow == FLOW_RETURN)
			return flow;
		if (flow == FLOW_BREAK)
			return FLOW_NORMAL;
		if (iteration && !iteration->children.empty())
			EvalEffect(*iteration->children[0]);
	}
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecReturn(const SemNode& node,
                                                   Frame& frame)
{
	frame.returned = true;
	if (node.children.empty())
		return FLOW_RETURN;
	const SemNode& value = *node.children[0];
	if (IsVoidType(frame.ret_type))
	{
		EvalEffect(value);
		return FLOW_RETURN;
	}
	if (IsReferenceType(frame.ret_type))
	{
		frame.ret_value.kind = EvalValue::EV_PTR;
		frame.ret_value.ptr = EvalLValue(value);
		frame.ret_value.type = frame.ret_type;
		return FLOW_RETURN;
	}
	TypePtr bare = RemoveTopCv(frame.ret_type);
	if (bare->kind == TK_CLASS)
	{
		if (!frame.ret_target_valid)
			throw NotConstant("class return target");
		EvalInit(value, frame.ret_target, bare);
		frame.ret_value.kind = EvalValue::EV_PTR;
		frame.ret_value.ptr = frame.ret_target;
		frame.ret_value.type = bare;
		return FLOW_RETURN;
	}
	frame.ret_value = Convert(EvalRead(value), bare);
	return FLOW_RETURN;
}

ConstEvalEngine::EFlow ConstEvalEngine::ExecStatement(const SemNode& node,
                                                      Frame& frame)
{
	Step();
	switch (node.kind)
	{
	case SN_COMPOUND_STATEMENT:
		return ExecCompound(node, frame);
	case SN_SIMPLE_DECLARATION:
		for (size_t i = 0; i < node.children.size(); i++)
			if (node.children[i]->kind == SN_VARIABLE)
				ExecLocalVariable(*node.children[i], frame);
		return FLOW_NORMAL;
	case SN_VARIABLE:
		ExecLocalVariable(node, frame);
		return FLOW_NORMAL;
	case SN_TYPE_ALIAS:
	case SN_FUNCTION_DECLARATION:
	case SN_DESTRUCTOR_ACTION:
		return FLOW_NORMAL;
	case SN_EXPRESSION_STATEMENT:
		EvalEffect(*node.children[0]);
		return FLOW_NORMAL;
	case SN_CONSTRUCTOR_ACTION:
		EvalConstructorAction(node, 0, TypePtr());
		return FLOW_NORMAL;
	case SN_STORAGE_COPY:
		CopyBytes(EvalLValue(*node.children[0]),
		          EvalLValue(*node.children[1]), node.value.bits);
		return FLOW_NORMAL;
	case SN_RETURN_STATEMENT:
		return ExecReturn(node, frame);
	case SN_IF_STATEMENT:
		return ExecIf(node, frame);
	case SN_WHILE_STATEMENT:
	case SN_DO_STATEMENT:
		return ExecLoop(node, frame);
	case SN_FOR_STATEMENT:
		return ExecFor(node, frame);
	case SN_BREAK_STATEMENT:
		return FLOW_BREAK;
	case SN_CONTINUE_STATEMENT:
		return FLOW_CONTINUE;
	default:
		throw NotConstant("statement form");
	}
}
