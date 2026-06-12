#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 object lifetime: declared-object initialization forms
// (direct, copy, value, aggregate), array element construction, and
// the synthesized field-wise aggregate constructor (8.5, 8.5.1, 12.2).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

// 13.1p2 parameter-list agreement of an out-of-class declarator with
// an in-class declaration.
bool SameAssignmentSignature(const Type& a, const Type& b)
{
	if (a.parameters.size() != b.parameters.size() ||
	    a.is_const != b.is_const || a.is_volatile != b.is_volatile)
		return false;
	for (size_t i = 0; i < a.parameters.size(); i++)
		if (!TypeEquals(a.parameters[i], b.parameters[i]))
			return false;
	return true;
}

}  // namespace

// --- object lifetime --------------------------------------------------------

SemNodePtr SemBinder::VariableObjectExpr(const ScopeBinding& binding)
{
	SemNodePtr object = MakeSemNode(SN_ID_EXPRESSION);
	object->name = binding.name;
	object->type = binding.type;
	object->category = VC_LVALUE;
	object->entity_scope = binding.home;
	object->entity_name = binding.name;
	return object;
}

void SemBinder::AppendAggregateInit(const ClassInfo& cls,
                                    const SemNode& target_proto,
                                    const AstExpr& braced,
                                    vector<SemNodePtr>& out)
{
	bf_units_written_.clear();
	size_t used = ConsumeAggregateItems(cls, target_proto,
	                                    braced.arguments, 0, true, out);
	if (used < braced.arguments.size())
		throw runtime_error("too many initializers for aggregate");
}

// 8.5.1 with brace elision: members initialize in declaration order
// from `items`, a nested braced item initializes one subaggregate
// fully, and a non-braced item starts an elided subaggregate that
// One class-type member inside an aggregate: its own braces
// initialize it fully (aggregate recursion or list-construction);
// otherwise an elided subaggregate or one copy-initializing item.
size_t SemBinder::ConsumeAggregateClassItem(const ClassInfo& member_cls,
                                            const ClassField& field,
                                            SemNodePtr member,
                                            const vector<AstExprPtr>& items,
                                            size_t at,
                                            vector<SemNodePtr>& out)
{
	const AstExpr* element = at < items.size() ? items[at].get() : 0;
	if (element && element->kind == EK_BRACED)
	{
		at++;
		if (member_cls.is_aggregate)
		{
			size_t used = ConsumeAggregateItems(
				member_cls, *member, element->arguments, 0,
				true, out);
			if (used < element->arguments.size())
				throw runtime_error("too many initializers "
				                    "for aggregate member");
		}
		else
		{
			vector<SemValue> values;
			for (size_t j = 0;
			     j < element->arguments.size(); j++)
				values.push_back(analyzer_.Analyze(
					*element->arguments[j]));
			int index = ResolveClassConstructor(
				member_cls, values, true, field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < values.size(); j++)
				arg_nodes.push_back(std::move(values[j].node));
			out.push_back(MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(std::move(member)),
				std::move(arg_nodes)));
		}
		return at;
	}
	if (element && member_cls.is_aggregate)
	{
		// 8.5.1p13: an expression convertible to the subaggregate's
		// type initializes the whole member; only otherwise does brace
		// elision consume items element-wise.
		SemValue probe = analyzer_.Analyze(*element);
		TypePtr probe_type = RemoveTopCv(probe.type);
		if (probe_type->kind == TK_CLASS &&
		    BaseClassDistance(probe_type->named, member_cls.entity) >= 0)
		{
			vector<SemValue> values;
			values.push_back(std::move(probe));
			int index = ResolveClassConstructor(member_cls, values, true,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			arg_nodes.push_back(std::move(values[0].node));
			out.push_back(MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(std::move(member)),
				std::move(arg_nodes)));
			return at + 1;
		}
	}
	if (member_cls.is_aggregate)
	{
		// Brace elision: the nested aggregate consumes the
		// following items in place.
		at = ConsumeAggregateItems(member_cls, *member, items,
		                           at, false, out);
		return at;
	}
	// A non-aggregate member without its own braces: one item
	// copy-initializes it (or it default-constructs).
	vector<SemValue> values;
	if (element)
	{
		at++;
		values.push_back(analyzer_.Analyze(*element));
	}
	if (!values.empty() && values[0].node &&
	    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == member_cls.entity)
	{
		SemNodePtr action = std::move(values[0].node);
		SemNode& call = *action->children[0];
		call.children.insert(
			call.children.begin() + 1,
			AddressOfNode(std::move(member)));
		out.push_back(std::move(action));
		return at;
	}
	int index = ResolveClassConstructor(member_cls, values,
	                                    true,
	                                    field.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < values.size(); j++)
		arg_nodes.push_back(std::move(values[j].node));
	out.push_back(MakeConstructorCall(
		member_cls, index, false,
		AddressOfNode(std::move(member)),
		std::move(arg_nodes)));
	return at;
}

// consumes following items. Returns the next unused position.
size_t SemBinder::ConsumeAggregateItems(const ClassInfo& cls,
                                        const SemNode& target_proto,
                                        const vector<AstExprPtr>& items,
                                        size_t at, bool top_braced,
                                        vector<SemNodePtr>& out)
{
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		// 8.5.1p15: a braced union initializer initializes the first
		// non-static data member only.
		if (cls.is_union && !out.empty())
			break;
		SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
		member->name = field.name;
		member->type = IsReferenceType(field.type) ? field.type->target
		                                           : field.type;
		member->member_ref = IsReferenceType(field.type);
		member->category = VC_LVALUE;
		member->member_offset = field.offset;
		member->is_bit_field = field.is_bit_field;
		member->bit_offset = field.bit_offset;
		member->bit_width = field.bit_width;
		member->children.push_back(CloneSemNode(target_proto));
		const AstExpr* element = at < items.size() ? items[at].get() : 0;
		TypePtr bare = RemoveTopCv(field.type);
		const ClassInfo* member_cls = bare->kind == TK_CLASS
			? unit_.classes.Find(bare->named) : 0;
		if (member_cls)
		{
			at = ConsumeAggregateClassItem(*member_cls, field,
			                               std::move(member), items, at,
			                               out);
			continue;
		}
		if (bare->kind == TK_ARRAY)
		{
			at = ConsumeArrayItems(field, *member, items, at,
			                       top_braced, out);
			continue;
		}
		ClassField row = field;
		if (element)
		{
			at++;
			SemValue value = analyzer_.Analyze(*element);
			if (!IsReferenceType(field.type))
				CheckListInitNarrowing(value, bare);
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 std::move(value)));
		}
		else
		{
			if (IsReferenceType(field.type))
				throw runtime_error("reference member is not "
				                    "initialized");
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 ZeroValue(bare)));
		}
	}
	return at;
}

// One array member inside an aggregate: a braced item initializes it
// fully; otherwise the elements consume following items (elision).
size_t SemBinder::ConsumeArrayItems(const ClassField& field,
                                    const SemNode& member_proto,
                                    const vector<AstExprPtr>& items,
                                    size_t at, bool top_braced,
                                    vector<SemNodePtr>& out)
{
	TypePtr array = RemoveTopCv(field.type);
	TypePtr element_type = RemoveTopCv(array->target);
	if (element_type->kind == TK_CLASS)
		throw OutsideBoundary("aggregate class array member");
	const AstExpr* element = at < items.size() ? items[at].get() : 0;
	if (element && element->kind == EK_LITERAL &&
	    element->literal_kind == PTK_LITERAL_ARRAY &&
	    IsIntegralType(element_type))
	{
		// 8.5.2: a string literal initializes the character array
		// element-wise (trailing elements zero).
		SemValue value = analyzer_.Analyze(*element);
		const string& bytes = value.node->string_bytes;
		unsigned long long width = TypeSize(element_type);
		if (bytes.size() / width > array->bound)
			throw runtime_error("string literal exceeds the array "
			                    "member");
		for (unsigned long long j = 0; j < array->bound; j++)
		{
			unsigned long long code = 0;
			if ((j + 1) * width <= bytes.size())
				code = LittleEndianValue(
					bytes.substr(j * width, width));
			ClassField element_field;
			element_field.name = field.name;
			element_field.type = element_type;
			SemValue item_value;
			item_value.node = MakeSemNode(SN_LITERAL);
			item_value.node->token = std::to_string(code);
			item_value.node->type = element_type;
			item_value.node->category = VC_PRVALUE;
			item_value.node->has_value = true;
			item_value.node->value = ConstValue(
				element_type->fundamental, code);
			item_value.type = element_type;
			out.push_back(MemberAssignAction(
				element_field,
				SubscriptNode(CloneSemNode(member_proto), j),
				std::move(item_value)));
		}
		return at + 1;
	}
	const vector<AstExprPtr>* source = &items;
	size_t inner_at = at;
	bool own_braces = element && element->kind == EK_BRACED;
	if (own_braces)
		source = &element->arguments;
	if (own_braces)
		inner_at = 0;
	for (unsigned long long j = 0; j < array->bound; j++)
	{
		ClassField element_field;
		element_field.name = field.name;
		element_field.type = element_type;
		SemNodePtr target =
			SubscriptNode(CloneSemNode(member_proto), j);
		const AstExpr* item_expr = inner_at < source->size()
			? (*source)[inner_at].get() : 0;
		if (item_expr && (own_braces || inner_at < source->size()))
		{
			if (!own_braces && item_expr->kind == EK_BRACED)
				throw OutsideBoundary("nested braces in an elided "
				                      "array member");
			inner_at++;
			SemValue value = analyzer_.Analyze(*item_expr);
			CheckListInitNarrowing(value, element_type);
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 std::move(value)));
		}
		else
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 ZeroValue(element_type)));
	}
	if (own_braces)
	{
		if (inner_at < source->size())
			throw runtime_error("too many initializers for array "
			                    "member");
		return at + 1;
	}
	return inner_at;
}

// The synthesized field-wise constructor that aggregate array elements
// call: one parameter per named field, stored in order.
TypePtr SemBinder::EnsureAggregateCtor(const ClassInfo& cls_in)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	vector<TypePtr> params;
	vector<string> names;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		if (cls.fields[i].name.empty())
			continue;
		params.push_back(AdjustParameterType(cls.fields[i].type));
		names.push_back(cls.fields[i].name);
	}
	TypePtr ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                     params, false);
	TypePtr adjusted = MethodAdjustedType(cls, ctor_type);
	if (cls.aggregate_ctor_built)
		return adjusted;
	cls.aggregate_ctor_built = true;
	const string& base_name = cls.members->name;
	Scope* fn_scope =
		model_.CreateScope(SCOPE_FUNCTION, base_name, cls.members);
	DeferredBody body;
	body.name = base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.composed.type = ctor_type;
	for (size_t i = 0; i < params.size(); i++)
	{
		ParameterInfo parameter;
		parameter.name = names[i];
		parameter.type = params[i];
		body.composed.parameters.push_back(parameter);
		ScopeBinding param_binding;
		param_binding.kind = SB_PARAMETER;
		param_binding.name = parameter.name;
		param_binding.type = parameter.type;
		AddBinding(*fn_scope, param_binding);
	}
	SemNodePtr item = BuildFunctionNode(body, SF_CONSTRUCTOR);
	SemNode* node = item.get();
	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	current_ = fn_scope;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = base_name;
	method_.this_type = node->type->parameters[0];
	bf_units_written_.clear();
	size_t param_at = 0;
	vector<SemNodePtr> actions;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		SemValue value;
		value.node = MakeSemNode(SN_ID_EXPRESSION);
		value.node->name = names[param_at];
		value.node->type = params[param_at];
		value.node->category = VC_LVALUE;
		value.node->entity_scope = fn_scope;
		value.node->entity_name = names[param_at];
		value.type = IsReferenceType(params[param_at])
			? params[param_at]->target : params[param_at];
		value.category = VC_LVALUE;
		actions.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                     std::move(value)));
		param_at++;
	}
	for (size_t i = 0; i < actions.size(); i++)
		node->children.push_back(std::move(actions[i]));
	current_ = saved_scope;
	method_ = saved_method;
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	unit_.deferred.push_back(std::move(item));
	return adjusted;
}

// A braced aggregate temporary (`T{items}` over an aggregate class):
// the synthesized field-wise constructor runs over the converted
// items; trailing fields value-initialize through zero arguments.
SemNodePtr SemBinder::MakeAggregateTemporary(const ClassInfo& cls_in,
                                             vector<SemValue> args)
{
	TypePtr adjusted = EnsureAggregateCtor(cls_in);
	const ClassInfo& cls = *unit_.classes.Find(cls_in.entity);
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::" + base_name;
	SemNodePtr action = MakeSemNode(SN_CONSTRUCTOR_ACTION);
	action->name = qualified;
	action->special = SF_CONSTRUCTOR;
	action->type = MakeNamedType(TK_CLASS, cls.entity);
	action->category = VC_PRVALUE;
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = MakeFundamentalType(FT_VOID);
	call->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = qualified;
	callee->type = adjusted;
	callee->entity_scope = cls.members;
	callee->entity_name = base_name;
	callee->is_method = true;
	callee->special = SF_CONSTRUCTOR;
	callee->unwind_no = true;
	call->children.push_back(std::move(callee));
	if (args.size() + 1 > adjusted->parameters.size())
		throw runtime_error("too many initializers for aggregate");
	for (size_t i = 0; i < args.size(); i++)
	{
		analyzer_.CopyInitialize(args[i], adjusted->parameters[i + 1],
		                         "aggregate item");
		call->children.push_back(std::move(args[i].node));
	}
	for (size_t i = args.size() + 1; i < adjusted->parameters.size(); i++)
	{
		TypePtr param = adjusted->parameters[i];
		if (IsReferenceType(param))
			throw runtime_error("reference member is not initialized");
		SemValue zero = ZeroValue(RemoveTopCv(param));
		call->children.push_back(std::move(zero.node));
	}
	action->children.push_back(std::move(call));
	if (unit_.classes.NeedsDestruction(cls))
	{
		action->needs_dtor = true;
		action->children.push_back(MakeTemporaryDtor(cls));
	}
	return action;
}

// A braced array of aggregates: each element runs the synthesized
// field-wise constructor at its byte offset (the action records the
// A declared class array: braced per-element construction (aggregate
// classes at block scope take the field-wise form), or the per-element
// default construction of an uninitialized array.
void SemBinder::AppendClassArrayInit(SemNode& item, ScopeBinding& binding,
                                     const AstInitializer* init,
                                     const ClassInfo& cls)
{
	TypePtr type = binding.type;

	if (init)
	{
		const AstExpr* braced = 0;
		if (init->kind == INIT_BRACED)
			braced = init->expr.get();
		else if (init->kind == INIT_EQ &&
		         init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		if (!braced)
			throw OutsideBoundary("class array initializer form");
		if (!cls.has_user_ctor && cls.is_aggregate &&
		    binding.home && binding.home->kind != SCOPE_NAMESPACE)
		{
			AppendAggregateArrayInit(item, binding, cls, *braced);
			return;
		}
		// Elements list-initialize through their constructors at
		// subscripted addresses.
		TypePtr completed = binding.type;
		if (!completed->bound_known)
		{
			completed = MakeArrayType(completed->target, true,
			                          braced->arguments.size());
			binding.type = completed;
			item.type = completed;
		}
		if (braced->arguments.size() > completed->bound)
			throw runtime_error("too many initializers for " +
			                    binding.name);
		for (size_t i = 0; i < braced->arguments.size(); i++)
		{
			const AstExpr* element = braced->arguments[i].get();
			vector<SemValue> values;
			if (element->kind == EK_BRACED)
				for (size_t j = 0;
				     j < element->arguments.size(); j++)
					values.push_back(analyzer_.Analyze(
						*element->arguments[j]));
			else
				values.push_back(analyzer_.Analyze(*element));
			if (values.size() == 1 && values[0].node &&
			    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
			    values[0].category == VC_PRVALUE &&
			    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
			    RemoveTopCv(values[0].type)->named == cls.entity)
			{
				// 12.8p31: the element temporary elides; the
				// constructor runs on the element directly.
				SemNodePtr action = std::move(values[0].node);
				action->needs_dtor = false;
				while (action->children.size() > 1 &&
				       action->children.back()->kind ==
				           SN_DESTRUCTOR_ACTION)
					action->children.pop_back();
				SemNode& call = *action->children[0];
				call.children.insert(
					call.children.begin() + 1,
					AddressOfNode(SubscriptNode(
						VariableObjectExpr(binding), i)));
				item.children.push_back(std::move(action));
				continue;
			}
			int index = ResolveClassConstructor(
				cls, values, true, binding.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < values.size(); j++)
				arg_nodes.push_back(std::move(values[j].node));
			item.children.push_back(MakeConstructorCall(
				cls, index, false,
				AddressOfNode(SubscriptNode(
					VariableObjectExpr(binding), i)),
				std::move(arg_nodes)));
		}
		// 8.5.1p7: elements beyond the initializer list
		// value-initialize.
		for (unsigned long long i = braced->arguments.size();
		     i < completed->bound; i++)
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(cls, no_args, false,
			                                    binding.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			item.children.push_back(MakeConstructorCall(
				cls, index, false,
				AddressOfNode(SubscriptNode(
					VariableObjectExpr(binding), i)),
				std::move(arg_nodes)));
		}
		return;
	}
	if (!cls.has_user_ctor && !unit_.classes.NeedsConstruction(cls))
		return;
	for (unsigned long long i = 0; i < type->bound; i++)
	{
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    binding.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t j = 0; j < no_args.size(); j++)
			arg_nodes.push_back(std::move(no_args[j].node));
		item.children.push_back(MakeConstructorCall(
			cls, index, false,
			AddressOfNode(SubscriptNode(VariableObjectExpr(binding),
			                            i)),
			std::move(arg_nodes)));
	}
	return;
}

// offset; the lowering shares one base address).
void SemBinder::AppendAggregateArrayInit(SemNode& item,
                                         ScopeBinding& binding,
                                         const ClassInfo& cls,
                                         const AstExpr& braced)
{
	TypePtr array = binding.type;
	if (!array->bound_known)
	{
		array = MakeArrayType(array->target, true,
		                      braced.arguments.size());
		binding.type = array;
		item.type = array;
	}
	if (braced.arguments.size() > array->bound)
		throw runtime_error("too many initializers for " + binding.name);
	TypePtr adjusted = EnsureAggregateCtor(cls);
	unsigned long long size = cls.size;
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::" + base_name;
	for (size_t i = 0; i < braced.arguments.size(); i++)
	{
		const AstExpr* element = braced.arguments[i].get();
		if (element->kind != EK_BRACED)
			throw OutsideBoundary("array aggregate element form");
		vector<SemValue> values;
		size_t value_at = 0;
		SemNodePtr action = MakeSemNode(SN_CONSTRUCTOR_ACTION);
		action->name = qualified;
		action->special = SF_CONSTRUCTOR;
		action->has_value = true;
		action->value = ConstValue(FT_UNSIGNED_LONG_INT, i * size);
		SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
		call->type = MakeFundamentalType(FT_VOID);
		call->category = VC_PRVALUE;
		SemNodePtr callee = MakeSemNode(SN_CALLEE);
		callee->name = qualified;
		callee->type = adjusted;
		callee->entity_scope = cls.members;
		callee->entity_name = base_name;
		callee->is_method = true;
		callee->special = SF_CONSTRUCTOR;
		callee->unwind_no = true;
		call->children.push_back(std::move(callee));
		for (size_t j = 0; j < element->arguments.size(); j++)
		{
			SemValue value = analyzer_.Analyze(*element->arguments[j]);
			if (value_at + 1 < adjusted->parameters.size())
				analyzer_.CopyInitialize(
					value,
					adjusted->parameters[value_at + 1],
					"aggregate element");
			value_at++;
			call->children.push_back(std::move(value.node));
		}
		if (element->arguments.size() + 1 > adjusted->parameters.size())
			throw runtime_error("too many initializers for array "
			                    "element");
		// Trailing fields value-initialize through zero arguments.
		for (size_t j = element->arguments.size() + 1;
		     j < adjusted->parameters.size(); j++)
		{
			TypePtr param = adjusted->parameters[j];
			SemValue zero = ZeroValue(
				IsReferenceType(param) ? param->target
				                       : RemoveTopCv(param));
			call->children.push_back(std::move(zero.node));
		}
		action->children.push_back(std::move(call));
		item.children.push_back(std::move(action));
	}
}

void SemBinder::AttachObjectLifetime(SemNode& item, ScopeBinding& binding,
                                     const AstInitializer* init,
                                     const DeclSpecifierInfo& specs)
{
	TypePtr type = binding.type;
	TypePtr inner = type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	inner = RemoveTopCv(inner);
	bool class_object = !IsReferenceType(type) &&
		inner->kind == TK_CLASS;
	if (!class_object)
	{
		if (init)
			AnalyzeVariableInit(item, binding, init);
		return;
	}
	if (specs.is_extern && !init)
		return;  // a declaration of an object defined elsewhere
	if (!inner->named->complete)
		throw runtime_error(inner->named->display +
		                    " is an incomplete type");
	const ClassInfo* cls = unit_.classes.Find(inner->named);
	if (!cls)
		throw runtime_error("class record missing for " +
		                    inner->named->display);
	AppendClassObjectInit(item, binding, init, *cls);
	// Destruction at scope exit / program shutdown.
	bool needs_dtor = unit_.classes.NeedsDestruction(*cls);
	if (!needs_dtor)
		return;
	if (!unit_.classes.DestructionHasEffects(*cls))
	{
		// The destructor is still potentially invoked (resolved and
		// access-checked); an effect-free chain emits no cleanup.
		MakeDestructorCall(*cls, false, SemNodePtr());
		return;
	}
	item.needs_dtor = true;
	if (type->kind == TK_ARRAY)
	{
		for (unsigned long long i = 0; i < type->bound; i++)
			item.children.push_back(MakeDestructorCall(
				*cls, false,
				AddressOfNode(SubscriptNode(VariableObjectExpr(binding),
				                            i))));
		return;
	}
	item.children.push_back(MakeDestructorCall(
		*cls, false, AddressOfNode(VariableObjectExpr(binding))));
}

void SemBinder::AppendClassObjectInit(SemNode& item, ScopeBinding& binding,
                                      const AstInitializer* init,
                                      const ClassInfo& cls)
{
	if (binding.type->kind == TK_ARRAY)
	{
		AppendClassArrayInit(item, binding, init, cls);
		return;
	}
	if (!init)
	{
		// Default-initialization. The PA12 dump pins the
		// constructor-action plus synthesized empty definition shape
		// for the trivial subset; richer classes resolve a real
		// constructor.
		if (!cls.has_user_ctor)
		{
			TypePtr ctor_type;
			EnsureDefaultConstructor(RemoveTopCv(binding.type),
			                         ctor_type);
		}
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    binding.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t j = 0; j < no_args.size(); j++)
			arg_nodes.push_back(std::move(no_args[j].node));
		item.children.push_back(MakeConstructorCall(
			cls, index, false,
			AddressOfNode(VariableObjectExpr(binding)),
			std::move(arg_nodes)));
		return;
	}
	// Initialized class object: direct (paren), list (braced), or
	// copy-initialization.
	vector<const AstExpr*> args;
	const AstExpr* braced = 0;
	bool copy_init = false;
	switch (init->kind)
	{
	case INIT_PAREN:
		for (size_t i = 0; i < init->args.size(); i++)
			args.push_back(init->args[i].get());
		break;
	case INIT_EQ:
		copy_init = true;
		if (init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		else
			args.push_back(init->expr.get());
		break;
	case INIT_BRACED:
		braced = init->expr.get();
		break;
	default:
		throw OutsideBoundary("class initializer form");
	}
	if (braced && cls.is_aggregate)
	{
		SemNodePtr proto = VariableObjectExpr(binding);
		vector<SemNodePtr> actions;
		AppendAggregateInit(cls, *proto, *braced, actions);
		for (size_t i = 0; i < actions.size(); i++)
			item.children.push_back(std::move(actions[i]));
		return;
	}
	vector<SemValue> values;
	if (braced)
		for (size_t i = 0; i < braced->arguments.size(); i++)
			values.push_back(analyzer_.Analyze(*braced->arguments[i]));
	else
		for (size_t i = 0; i < args.size(); i++)
			values.push_back(analyzer_.Analyze(*args[i]));
	if (copy_init && !braced && values.size() == 1 &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    BaseClassDistance(RemoveTopCv(values[0].type)->named,
	                      cls.entity) >= 0)
		// PA16 copy-initialization from a class value: the copy/move
		// constructor wraps the source (unless the source already
		// constructs in place); the elision below targets the object.
		analyzer_.CopyInitialize(values[0],
		                         MakeNamedType(TK_CLASS, cls.entity),
		                         "initialization");
	if (values.size() == 1 && values[0].node &&
	    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == cls.entity)
	{
		// 12.8p31: the temporary elides; the constructor runs directly
		// on the declared object.
		SemNodePtr action = std::move(values[0].node);
		// The elided temporary's own cleanup does not apply; the
		// declared object's lifetime governs.
		action->needs_dtor = false;
		while (action->children.size() > 1 &&
		       action->children.back()->kind == SN_DESTRUCTOR_ACTION)
			action->children.pop_back();
		SemNode& call = *action->children[0];
		call.children.insert(
			call.children.begin() + 1,
			AddressOfNode(VariableObjectExpr(binding)));
		item.children.push_back(std::move(action));
		return;
	}
	if (values.size() == 1 && values[0].node &&
	    values[0].category == VC_PRVALUE &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == cls.entity)
	{
		// A same-class prvalue initializer (call result, conditional)
		// constructs the declared object directly (copy elision).
		item.children.push_back(std::move(values[0].node));
		return;
	}
	int index = ResolveClassConstructor(cls, values, copy_init,
	                                    binding.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < values.size(); i++)
		arg_nodes.push_back(std::move(values[i].node));
	item.children.push_back(MakeConstructorCall(
		cls, index, false, AddressOfNode(VariableObjectExpr(binding)),
		std::move(arg_nodes)));
}

void SemBinder::CheckQualifiedDefinitionScope(const Scope* declaring)
{
	if (declaring->kind != SCOPE_NAMESPACE &&
	    declaring->kind != SCOPE_CLASS)
		throw OutsideBoundary("qualified function definition scope");
}

// A qualified declarator at namespace scope: the out-of-class
// definition of a static data member (9.4.2p2).
void SemBinder::BindQualifiedDeclarator(const DeclSpecifierInfo& specs,
                                        const AstInitDeclarator& declarator,
                                        const DeclaratorInfo& composed)
{
	if (composed.type->kind == TK_FUNCTION)
	{
		// PA16: an out-of-class `= default` assignment operator
		// synthesizes its definition as a source-owned strong one.
		string name = DeclaredFunctionName(composed.id->parts.back());
		Scope* owner = ResolvePrefixScope(*composed.id);
		if (owner->kind != SCOPE_CLASS || name != "operator =" ||
		    !declarator.init || declarator.init->kind != INIT_DEFAULT)
			throw OutsideBoundary("qualified function declarator");
		const NamedTypeInfo* entity = model_.ScopeEntity(owner);
		ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
		ScopeBinding* binding = FindOwnBinding(*owner, name);
		if (!cls || !binding || binding->kind != SB_FUNCTION)
			throw runtime_error("defaulted operator= matches no "
			                    "declaration");
		size_t count = binding->overloads.size() + 1;
		size_t index = count;
		for (size_t i = 0; i < count; i++)
		{
			const TypePtr& declared =
				i == 0 ? binding->type : binding->overloads[i - 1];
			if (SameAssignmentSignature(*declared, *composed.type))
				index = i;
		}
		if (index == count)
			throw runtime_error("defaulted operator= matches no "
			                    "declaration");
		binding->fn_defaulted.resize(count, false);
		binding->fn_defaulted[index] = true;
		const TypePtr& param =
			(index == 0 ? binding->type : binding->overloads[index - 1])
				->parameters[0];
		if (param->kind == TK_RVALUE_REFERENCE)
			cls->move_assign_index = (int)index;
		else
			cls->copy_assign_index = (int)index;
		BuildAssignSpecial(*cls, index, true);
		return;
	}
	Scope* declaring = ResolvePrefixScope(*composed.id);
	if (declaring->kind != SCOPE_CLASS &&
	    declaring->kind != SCOPE_NAMESPACE)
		throw OutsideBoundary("qualified declarator scope");
	const string& name = PartName(composed.id->parts.back());
	ScopeBinding* member = FindOwnBinding(*declaring, name);
	if (!member || member->kind != SB_VARIABLE)
		throw runtime_error(name + " is not a declared object");
	member->type = MergeRedeclaredType(member->type, composed.type);
	// The definition emits like a namespace-scope object owned by the
	// declaring scope. A qualified definition completes an earlier
	// (extern) declaration, so const-ness does not make it internal.
	SemNode* item = AppendItem(SN_VARIABLE);
	item->name = QualifiedScopePath(declaring) + name;
	item->type = member->type;
	item->entity_scope = declaring;
	item->entity_name = name;
	item->is_static_decl = specs.is_static;
	item->is_thread_local_decl = specs.is_thread_local;
	item->is_extern_decl = declaring->kind == SCOPE_NAMESPACE;
	AttachObjectLifetime(*item, *member, declarator.init.get(), specs);
}

// 6.8p1 disambiguation with name lookup: `begin(a);` is only a
// declaration when `begin` names a type; when it names a function the
// statement is the call expression instead. The parser commits to the
// declaration form, so the binder re-reads the shape here.
bool SemBinder::TryVexingCallRecovery(const AstDecl& decl)
{
	if (decl.kind != DK_SIMPLE || decl.declarators.size() != 1 ||
	    decl.specifiers.size() != 1 ||
	    decl.specifiers[0].kind != SPEC_TYPE_NAME)
		return false;
	const AstName& callee_name = decl.specifiers[0].name;
	if (TryResolveTypeFromName(callee_name))
		return false;
	const ScopeBinding* fn = 0;
	try
	{
		const NamedTypeInfo* member_class = 0;
		fn = ResolveValue(callee_name, member_class);
	}
	catch (const std::exception&)
	{
		return false;
	}
	if (!fn || fn->kind != SB_FUNCTION)
		return false;
	const AstInitDeclarator& declarator = decl.declarators[0];
	if (declarator.init || !declarator.declarator ||
	    declarator.declarator->items.size() != 1 ||
	    declarator.declarator->items[0].kind != DI_NESTED)
		return false;
	const AstDeclarator* inner = declarator.declarator->items[0].nested.get();
	if (!inner || inner->items.size() != 1 ||
	    inner->items[0].kind != DI_ID ||
	    !inner->items[0].name.IsPlainIdentifier())
		return false;
	// Synthesize `callee(argument)` over owned AST nodes.
	AstExprPtr callee(new AstExpr(EK_ID));
	callee->name.global_scope = callee_name.global_scope;
	for (size_t i = 0; i < callee_name.parts.size(); i++)
	{
		AstNamePart part;
		part.kind = NP_IDENTIFIER;
		part.identifier = callee_name.parts[i].identifier;
		callee->name.parts.push_back(std::move(part));
	}
	AstExprPtr argument(new AstExpr(EK_ID));
	AstNamePart arg_part;
	arg_part.kind = NP_IDENTIFIER;
	arg_part.identifier = inner->items[0].name.parts[0].identifier;
	argument->name.parts.push_back(std::move(arg_part));
	AstExprPtr call(new AstExpr(EK_CALL));
	call->operands.push_back(std::move(callee));
	call->arguments.push_back(std::move(argument));
	SemNode* item = AppendItem(SN_EXPRESSION_STATEMENT);
	SemValue value = analyzer_.Analyze(*call);
	item->children.push_back(std::move(value.node));
	recovered_exprs_.push_back(std::move(call));
	return true;
}

bool SemBinder::InClassContextOrFriend(const NamedTypeInfo* cls)
{
	const ClassInfo* naming = unit_.classes.Find(cls);
	if (!naming)
		return false;
	vector<const ClassInfo*> contexts;
	if (method_.cls)
		contexts.push_back(method_.cls);
	if (method_.lexical_cls)
		contexts.push_back(method_.lexical_cls);
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS)
			if (const NamedTypeInfo* entity = model_.ScopeEntity(scope))
				if (const ClassInfo* context = unit_.classes.Find(entity))
					contexts.push_back(context);
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i]->members == naming->members)
			return true;
		for (size_t j = 0; j < naming->friend_classes.size(); j++)
			if (naming->friend_classes[j] == contexts[i]->entity)
				return true;
	}
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < naming->friend_functions.size(); i++)
			if (naming->friend_functions[i].first == method_.fn_owner &&
			    naming->friend_functions[i].second == method_.fn_name)
				return true;
	return false;
}

TypePtr SemBinder::ResolveTypeName(const AstName& name)
{
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	if (!found)
		throw runtime_error("undeclared type name " + TerminalName(name));
	if (found->kind != SB_TYPE && found->kind != SB_TYPE_ALIAS)
		throw runtime_error(found->name + " does not name a type");
	if (found->home && found->home->kind == SCOPE_CLASS)
	{
		const NamedTypeInfo* entity = model_.ScopeEntity(found->home);
		if (!entity || !InClassContextOrFriend(entity))
			CheckMemberAccess(found->home, found->access, found->name);
	}
	return found->type;
}
