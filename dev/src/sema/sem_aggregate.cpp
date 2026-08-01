#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// The synthesized field-wise aggregate constructor and aggregate
// temporaries (8.5.1, 12.2), split from sem_lifetime.cpp: the
// per-arity constructor covers a leading prefix of the named fields
// and value-initializes the rest, and PA34 designated arguments
// realign to their fields before the cover is chosen.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

}  // namespace

// The named fields of an aggregate and how many leading parameters
// the synthesized per-arity constructor must take: at least the
// provided count, extended over every field the body cannot
// value-initialize internally. A copyable omitted class member
// arrives as a materialized by-value argument (the pinned PA24
// shape); PA25: one whose copy is unusable value-initializes inside
// the synthesized body instead.
size_t SemBinder::AggregateCtorCover(ClassInfo& cls, size_t provided,
                                     vector<const ClassField*>& named)
{
	size_t non_scalar = 0;  // one past the last named field the ctor
	                        // cannot value-initialize internally
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		if (cls.fields[i].name.empty())
			continue;
		named.push_back(&cls.fields[i]);
		TypePtr bare = RemoveTopCv(cls.fields[i].type);
		bool internal_class_init = false;
		if (bare->kind == TK_CLASS)
		{
			const ClassInfo* member = unit_.classes.Find(bare->named);
			if (member)
			{
				DeclareImplicitSpecialMembers(
					unit_.classes.Create(bare->named));
				internal_class_init = true;
				for (size_t c = 0; c < member->ctors.size(); c++)
					if (member->ctors[c].kind == CK_COPY &&
					    !member->ctors[c].deleted)
						internal_class_init = false;
			}
		}
		if ((bare->kind == TK_CLASS && !internal_class_init) ||
		    bare->kind == TK_ARRAY ||
		    IsReferenceType(cls.fields[i].type))
			non_scalar = named.size();
	}
	size_t cover = provided > non_scalar ? provided : non_scalar;
	if (cover > named.size())
		cover = named.size();
	return cover;
}

// The synthesized field-wise constructor that aggregate temporaries
// and array elements call: one parameter per covered named field,
// stored in order; named fields beyond the cover (an omitted scalar
// tail) zero-initialize inside the body. The cover spans the provided
// initializers extended through the last non-scalar field (class and
// array members always take a parameter the call site materializes),
// so each distinct use arity gets its own signature.
TypePtr SemBinder::EnsureAggregateCtor(const ClassInfo& cls_in,
                                       size_t provided)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	vector<const ClassField*> named;
	size_t cover = AggregateCtorCover(cls, provided, named);
	vector<TypePtr> params;
	vector<string> names;
	for (size_t i = 0; i < cover; i++)
	{
		// An array member keeps its array type in the signature (the
		// deterministic mangled identity); the ABI layer passes it as a
		// decayed pointer to the caller-materialized argument array.
		TypePtr bare = RemoveTopCv(named[i]->type);
		params.push_back(bare->kind == TK_ARRAY
		                 ? bare
		                 : AdjustParameterType(named[i]->type));
		names.push_back(named[i]->name);
	}
	TypePtr ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                     params, false);
	TypePtr adjusted = MethodAdjustedType(cls, ctor_type);
	if (cls.aggregate_ctor_covers[cover])
		return adjusted;
	cls.aggregate_ctor_covers[cover] = true;
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
		// 8.3.5p5: within the body an array parameter is its adjusted
		// pointer (the decayed spill slot holds the caller's address).
		param_binding.type = parameter.type->kind == TK_ARRAY
			? MakePointerType(parameter.type->target, false, false)
			: parameter.type;
		AddBinding(*fn_scope, param_binding);
	}
	SemNodePtr item = BuildFunctionNode(body, SF_CONSTRUCTOR);
	item->synthesized = true;
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
	vector<SemNodePtr> actions;
	for (size_t i = 0; i < named.size(); i++)
	{
		// 8.5.1p15: a union initializes exactly one member (the
		// covered one, or the first when nothing is provided).
		if (cls.is_union && i >= (cover ? cover : 1))
			break;
		const ClassField& field = *named[i];
		SemValue value;
		if (i < cover)
		{
			// An array parameter's spill slot holds the decayed
			// pointer; the body reads it as pointer-to-element.
			TypePtr read_type = params[i]->kind == TK_ARRAY
				? MakePointerType(params[i]->target, false, false)
				: params[i];
			value.node = MakeSemNode(SN_ID_EXPRESSION);
			value.node->name = names[i];
			value.node->type = read_type;
			value.node->category = VC_LVALUE;
			value.node->entity_scope = fn_scope;
			value.node->entity_name = names[i];
			value.type = IsReferenceType(read_type)
				? read_type->target : read_type;
			value.category = VC_LVALUE;
		}
		else
		{
			TypePtr bare = RemoveTopCv(field.type);
			const ClassInfo* member_cls = bare->kind == TK_CLASS
				? unit_.classes.Find(bare->named) : 0;
			if (member_cls)
			{
				// 8.5.1p7: an omitted class member value-initializes
				// in place (8.5p10).
				AppendClassMemberInit(field, *member_cls, 0,
				                      vector<const AstExpr*>(),
				                      actions);
				continue;
			}
			// 8.5.1p7: the omitted scalar tail value-initializes.
			value = ZeroValue(RemoveTopCv(field.type));
		}
		actions.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                     std::move(value)));
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
// PA34: designated arguments realign to their named fields, with
// explicitly value-initialized holes between them (8.5.1p15 keeps a
// union to its one covered member).
void SemBinder::RealignDesignatedArguments(const ClassInfo& cls_in,
                                           vector<SemValue>& args)
{
	bool designated = false;
	for (size_t i = 0; i < args.size(); i++)
		if (!args[i].designator.empty())
			designated = true;
	if (!designated)
		return;
	const ClassInfo* cls = unit_.classes.Find(cls_in.entity);
	if (!cls)
		throw runtime_error("aggregate class record missing");
	if (cls->is_union)
	{
		// The synthesized union constructor covers only the leading
		// member.
		string first;
		for (size_t f = 0; f < cls->fields.size(); f++)
			if (!cls->fields[f].name.empty())
			{
				first = cls->fields[f].name;
				break;
			}
		if (args.size() != 1 || args[0].designator != first)
			throw OutsideBoundary("designated union member form");
		args[0].designator.clear();
		return;
	}
	vector<SemValue> aligned;
	size_t at = 0;
	for (size_t f = 0; f < cls->fields.size() && at < args.size(); f++)
	{
		const ClassField& field = cls->fields[f];
		if (field.name.empty())
			continue;
		SemValue& next = args[at];
		if (!next.designator.empty() && next.designator != field.name)
		{
			// A hole: the field value-initializes in place.
			if (IsReferenceType(field.type))
				throw runtime_error("reference member is not "
				                    "initialized");
			TypePtr bare = RemoveTopCv(field.type);
			if (bare->kind == TK_CLASS)
			{
				vector<AstExprPtr> no_args;
				aligned.push_back(analyzer_.MakeTemporaryObject(
					bare, no_args, false));
			}
			else
				aligned.push_back(ZeroValue(bare));
			continue;
		}
		next.designator.clear();
		aligned.push_back(std::move(next));
		at++;
	}
	if (at < args.size())
		throw runtime_error("designated initializer order does not "
		                    "match the member order");
	args.swap(aligned);
}

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
		if (!cls.has_user_ctor && cls.is_aggregate)
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
			AppendArrayElementInit(item, binding, cls, i,
			                       braced->arguments[i].get());
		// 8.5.1p7: elements beyond the initializer list
		// value-initialize.
		for (unsigned long long i = braced->arguments.size();
		     i < completed->bound; i++)
			AppendArrayElementInit(item, binding, cls, i, 0);
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

// An array of aggregates: each element calls the shared full-cover
// constructor (member/local contexts ride one base address with an
// offset; a namespace-scope array's actions carry their subscripted
// element address explicitly).
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
	// Array elements share one full-cover signature; omitted element
	// members pad with zero arguments at the call sites.
	TypePtr adjusted = EnsureAggregateCtor(cls, (size_t)-1);
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
			const AstExpr& arg = *element->arguments[j];
			TypePtr param = value_at + 1 < adjusted->parameters.size()
				? adjusted->parameters[value_at + 1] : TypePtr();
			SemValue value;
			if (param && RemoveTopCv(param)->kind == TK_ARRAY &&
			    arg.kind == EK_BRACED)
			{
				// An array member's braced sub-list materializes an
				// argument array the call passes by decayed address.
				TypePtr dest = RemoveTopCv(param);
				value.node = analyzer_.AnalyzeBracedInit(
					arg.arguments, dest);
				value.type = dest;
				value.category = VC_LVALUE;
			}
			else
			{
				value = analyzer_.Analyze(arg);
				if (param)
					analyzer_.CopyInitialize(value, param,
					                         "aggregate element");
			}
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
			if (RemoveTopCv(param)->kind == TK_ARRAY)
			{
				// An omitted array member zero-fills its argument
				// array (an empty braced list).
				SemNodePtr list = MakeSemNode(SN_BRACED_INIT_LIST);
				list->type = RemoveTopCv(param);
				list->category = VC_LVALUE;
				call->children.push_back(std::move(list));
				continue;
			}
			SemValue zero = ZeroValue(
				IsReferenceType(param) ? param->target
				                       : RemoveTopCv(param));
			call->children.push_back(std::move(zero.node));
		}
		// The shared-base offset form serves member/local contexts;
		// an array emitted at namespace scope - a namespace variable
		// or a static data member definition (whose binding home is
		// its class scope) - carries the subscripted element address
		// explicitly, because its actions may clone into the
		// @__cppgm_init helper where no enclosing object supplies the
		// base.
		if (binding.home && (binding.home->kind == SCOPE_NAMESPACE ||
		                     binding.home->kind == SCOPE_CLASS))
		{
			call->children.insert(
				call->children.begin() + 1,
				AddressOfNode(SubscriptNode(
					VariableObjectExpr(binding), i)));
			action->ctor_addressed = true;
			action->has_value = false;
		}
		action->children.push_back(std::move(call));
		item.children.push_back(std::move(action));
	}
}

SemNodePtr SemBinder::MakeAggregateTemporary(const ClassInfo& cls_in,
                                             vector<SemValue> args)
{
	RealignDesignatedArguments(cls_in, args);
	TypePtr adjusted = EnsureAggregateCtor(cls_in, args.size());
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
		if (RemoveTopCv(param)->kind == TK_ARRAY)
		{
			// An omitted array member zero-fills its argument array
			// (an empty braced list).
			SemNodePtr list = MakeSemNode(SN_BRACED_INIT_LIST);
			list->type = RemoveTopCv(param);
			list->category = VC_LVALUE;
			call->children.push_back(std::move(list));
			continue;
		}
		if (RemoveTopCv(param)->kind == TK_CLASS)
		{
			// 8.5.1p7: an omitted class member value-initializes; the
			// call site materializes the temporary it passes by value.
			vector<AstExprPtr> no_args;
			SemValue filled = analyzer_.MakeTemporaryObject(
				RemoveTopCv(param), no_args, false);
			call->children.push_back(std::move(filled.node));
			continue;
		}
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
