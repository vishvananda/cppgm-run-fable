#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 constructor and destructor synthesis: mem-initializer
// analysis, default member initialization, destructor epilogues, and
// the implicitly-defined special members (12.1, 12.4, 12.6.2).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

}  // namespace

// --- constructor member initialization -------------------------------------

// 12.6.2 class-type member: the aggregate braced form, 8.5p10
// value-initialization, or direct/list-initialization by constructor.
void SemBinder::AppendClassMemberInit(const ClassField& field,
                                      const ClassInfo& member_cls,
                                      const AstExpr* braced,
                                      const vector<const AstExpr*>& args,
                                      vector<SemNodePtr>& out)
{
	TypePtr bare = RemoveTopCv(field.type);
	if (braced && member_cls.is_aggregate)
	{
		SemNodePtr proto = ThisFieldExpr(field);
		AppendAggregateInit(member_cls, *proto, *braced, out);
		return;
	}
	if (!braced && args.empty())
	{
		// 8.5p10 `member()`: zero-initialization, then the default
		// constructor when one is needed.
		SemNodePtr zero_stmt = MakeSemNode(SN_EXPRESSION_STATEMENT);
		SemNodePtr assign = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
		assign->type = bare;
		assign->category = VC_LVALUE;
		assign->has_op = true;
		assign->op = OP_ASS;
		assign->op_spelling = "=";
		if (member_cls.has_user_ctor ||
		    unit_.classes.NeedsConstruction(member_cls))
		{
			// The zero-fill folds into the constructor action so both
			// share one member address.
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			SemNodePtr action = MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				std::move(arg_nodes));
			action->has_value = true;
			action->value = ConstValue(FT_UNSIGNED_LONG_INT,
			                           member_cls.size);
			out.push_back(std::move(action));
			return;
		}
		SemValue zero = ZeroValue(MakeFundamentalType(FT_INT));
		assign->children.push_back(ThisFieldExpr(field));
		assign->children.push_back(std::move(zero.node));
		zero_stmt->children.push_back(std::move(assign));
		out.push_back(std::move(zero_stmt));
		return;
	}
	{
		// Direct- or list-initialization by constructor.
		vector<SemValue> values;
		if (braced)
			for (size_t i = 0; i < braced->arguments.size(); i++)
				values.push_back(analyzer_.Analyze(*braced->arguments[i]));
		else
			for (size_t i = 0; i < args.size(); i++)
				values.push_back(analyzer_.Analyze(*args[i]));
		if (values.size() == 1 && values[0].node &&
		    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
		    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
		    RemoveTopCv(values[0].type)->named == member_cls.entity)
		{
			SemNodePtr action = std::move(values[0].node);
			SemNode& call = *action->children[0];
			call.children.insert(
				call.children.begin() + 1,
				AddressOfNode(ThisFieldExpr(field)));
			action->ctor_addressed = true;
			out.push_back(std::move(action));
			return;
		}
		int index = ResolveClassConstructor(member_cls, values, false,
		                                    field.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < values.size(); i++)
			arg_nodes.push_back(std::move(values[i].node));
		out.push_back(MakeConstructorCall(
			member_cls, index, false,
			AddressOfNode(ThisFieldExpr(field)), std::move(arg_nodes)));
		return;
	}
}

void SemBinder::AppendMemberInit(const ClassInfo& cls,
                                 const ClassField& field,
                                 const AstInitializer* init,
                                 vector<SemNodePtr>& out)
{
	(void)cls;
	// Collect the initializer's argument expressions.
	vector<const AstExpr*> args;
	const AstExpr* braced = 0;
	if (init->kind == INIT_PAREN)
	{
		for (size_t i = 0; i < init->args.size(); i++)
			args.push_back(init->args[i].get());
	}
	else if (init->kind == INIT_EQ)
	{
		if (init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		else
			args.push_back(init->expr.get());
	}
	else if (init->kind == INIT_BRACED)
		braced = init->expr.get();
	else
		throw OutsideBoundary("member initializer form");

	TypePtr bare = RemoveTopCv(field.type);
	const ClassInfo* member_cls = bare->kind == TK_CLASS
		? unit_.classes.Find(bare->named) : 0;
	if (member_cls)
	{
		AppendClassMemberInit(field, *member_cls, braced, args, out);
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
		if (!braced && args.empty())
		{
			// 8.5p10: `arr()` value-initializes every element.
			TypePtr element = RemoveTopCv(bare->target);
			if (element->kind == TK_CLASS)
				throw OutsideBoundary("class array member initializer");
			for (unsigned long long i = 0; i < bare->bound; i++)
			{
				ClassField element_field;
				element_field.name = field.name;
				element_field.type = element;
				out.push_back(MemberAssignAction(
					element_field,
					SubscriptNode(ThisFieldExpr(field), i),
					ZeroValue(element)));
			}
			return;
		}
		if (!braced)
			throw OutsideBoundary("array member initializer form");
		AppendArrayMemberInit(field, braced, out);
		return;
	}
	// Scalar / reference / pointer member.
	if (braced)
	{
		if (braced->arguments.size() > 1)
			throw runtime_error("too many initializers for " + field.name);
		if (braced->arguments.empty())
		{
			out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
			                                 ZeroValue(bare)));
			return;
		}
		SemValue value = analyzer_.Analyze(*braced->arguments[0]);
		CheckListInitNarrowing(value, bare);
		out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                 std::move(value)));
		return;
	}
	if (args.empty())
	{
		// 8.5p10 `x()` value-initialization of a scalar member.
		if (IsReferenceType(field.type))
			throw runtime_error("reference member requires a binding");
		out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                 ZeroValue(bare)));
		return;
	}
	if (args.size() != 1)
		throw runtime_error("too many initializers for " + field.name);
	SemValue value = analyzer_.Analyze(*args[0]);
	out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
	                                 std::move(value)));
}

// A zero-valued prvalue of the scalar type (value-initialization).
SemValue SemBinder::ZeroValue(const TypePtr& type)
{
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = "0";
	value.category = VC_PRVALUE;
	if (type->kind == TK_POINTER || IsNullPtrType(type))
	{
		value.type = type;
		value.node->type = type;
		value.node->null_pointer = true;
		return value;
	}
	EFundamentalType fundamental = type->kind == TK_ENUM
		? type->named->enum_underlying
		: type->fundamental;
	value.type = type;
	value.node->type = type;
	value.node->has_value = true;
	value.node->value = ConstValue(fundamental, 0);
	value.null_pointer_literal = IsIntegralType(type);
	return value;
}

// 8.5.4p7 narrowing rejection for the list-initialization forms the
// tests pin: floating -> integral, integral -> floating (non-constant),
// and width-losing integral conversions of non-constant values.
void SemBinder::CheckListInitNarrowing(const SemValue& value,
                                       const TypePtr& dest)
{
	if (!IsArithmeticType(dest) || !IsArithmeticType(value.type))
		return;
	bool from_float = IsFloatingFundamental(value.type->fundamental);
	bool to_float = IsFloatingFundamental(dest->fundamental);
	if (from_float && !to_float)
		throw runtime_error("narrowing conversion in list-initialization");
	bool constant = value.node && value.node->has_value;
	if (constant)
	{
		// A constant that converts back unchanged does not narrow.
		ConstValue converted =
			ConvertConstValue(value.node->value, dest->fundamental);
		ConstValue back =
			ConvertConstValue(converted, value.node->value.type);
		if (back.bits != value.node->value.bits)
			throw runtime_error("narrowing conversion in "
			                    "list-initialization");
		return;
	}
	if (!from_float && to_float)
		throw runtime_error("narrowing conversion in list-initialization");
	if (!from_float && !to_float &&
	    (TypeSize(dest) < TypeSize(value.type) ||
	     (TypeSize(dest) == TypeSize(value.type) &&
	      IsSignedIntegralFundamental(dest->fundamental) !=
	      IsSignedIntegralFundamental(value.type->fundamental))))
		throw runtime_error("narrowing conversion in list-initialization");
}

void SemBinder::AppendArrayMemberInit(const ClassField& field,
                                      const AstExpr* braced,
                                      vector<SemNodePtr>& out)
{
	TypePtr array = RemoveTopCv(field.type);
	TypePtr element = RemoveTopCv(array->target);
	if (element->kind == TK_CLASS)
		throw OutsideBoundary("class array member initializer");
	if (braced->arguments.size() > array->bound)
		throw runtime_error("too many initializers for " + field.name);
	for (unsigned long long i = 0; i < array->bound; i++)
	{
		ClassField element_field;
		element_field.name = field.name;
		element_field.type = element;
		SemNodePtr target = SubscriptNode(ThisFieldExpr(field), i);
		if (i < braced->arguments.size())
		{
			SemValue value = analyzer_.Analyze(*braced->arguments[i]);
			CheckListInitNarrowing(value, element);
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 std::move(value)));
		}
		else
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 ZeroValue(element)));
	}
}

void SemBinder::AppendFieldDefaultInit(const ClassInfo& cls,
                                       const ClassField& field,
                                       vector<SemNodePtr>& out)
{
	if (field.default_init)
	{
		AppendMemberInit(cls, field, field.default_init, out);
		return;
	}
	TypePtr bare = RemoveTopCv(field.type);
	if (bare->kind == TK_CLASS)
	{
		const ClassInfo* member_cls = unit_.classes.Find(bare->named);
		if (!member_cls)
			return;
		if (member_cls->has_user_ctor ||
		    unit_.classes.NeedsConstruction(*member_cls))
		{
			if (!unit_.classes.DefaultConstructionHasEffects(
					*member_cls))
			{
				AppendElidedCtorDemand(*member_cls, false, out);
				return;
			}
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			out.push_back(MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				std::move(arg_nodes)));
		}
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
		TypePtr element = RemoveTopCv(bare->target);
		while (element->kind == TK_ARRAY)
			element = RemoveTopCv(element->target);
		if (element->kind != TK_CLASS)
			return;
		const ClassInfo* member_cls = unit_.classes.Find(element->named);
		if (!member_cls || (!member_cls->has_user_ctor &&
		                    !unit_.classes.NeedsConstruction(*member_cls)))
			return;
		if (!unit_.classes.DefaultConstructionHasEffects(*member_cls))
		{
			AppendElidedCtorDemand(*member_cls, false, out);
			return;
		}
		if (bare->target->kind == TK_ARRAY)
			throw OutsideBoundary("multidimensional class member array");
		for (unsigned long long i = 0; i < bare->bound; i++)
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			out.push_back(MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(SubscriptNode(ThisFieldExpr(field), i)),
				std::move(arg_nodes)));
		}
		return;
	}
	if (IsReferenceType(field.type))
		throw runtime_error("reference member is not initialized");
}

// A full-expression temporary's destructor action: no address subtree
// (the lowering destroys the materialized slot it created).
// 12.6.2p8 implicit default-initialization of the base subobject. A
// chain that performs no observable work is elided from the synthesized
// body (the reference emission); the user-provided constructors it
// would have reached are still resolved and demanded.
void SemBinder::AppendBaseDefaultInit(const ClassInfo& cls,
                                      vector<SemNodePtr>& out)
{
	const ClassInfo& base = *cls.base;
	if (!base.has_user_ctor && !unit_.classes.NeedsConstruction(base))
		return;
	if (!unit_.classes.DefaultConstructionHasEffects(base))
	{
		AppendElidedCtorDemand(base, true, out);
		return;
	}
	vector<SemValue> no_args;
	int index = ResolveClassConstructor(base, no_args, false,
	                                    "base subobject");
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < no_args.size(); j++)
		arg_nodes.push_back(std::move(no_args[j].node));
	out.push_back(MakeConstructorCall(base, index, true,
	                                  ThisBaseAddress(cls),
	                                  std::move(arg_nodes)));
}

// The demand side of an elided subobject chain: the nearest
// user-provided default constructors are resolved (each in its own
// implicit constructor's access context) and recorded as elided
// actions, so the lowering still emits the demanded definitions.
void SemBinder::AppendElidedCtorDemand(const ClassInfo& cls,
                                       bool base_entry,
                                       vector<SemNodePtr>& out)
{
	if (cls.has_user_ctor)
	{
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    "base subobject");
		SemNodePtr action = MakeConstructorCall(cls, index, base_entry,
		                                        SemNodePtr(),
		                                        vector<SemNodePtr>());
		action->elided = true;
		out.push_back(std::move(action));
		return;
	}
	MethodContext saved = method_;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = saved.fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = cls.members->name;
	try
	{
		if (cls.base)
			AppendElidedCtorDemand(*cls.base, true, out);
		for (size_t i = 0; i < cls.fields.size(); i++)
		{
			const ClassInfo* member =
				unit_.classes.MemberClass(cls.fields[i].type);
			if (member && (member->has_user_ctor ||
			               unit_.classes.NeedsConstruction(*member)))
				AppendElidedCtorDemand(*member, false, out);
		}
	}
	catch (...)
	{
		method_ = saved;
		throw;
	}
	method_ = saved;
}

SemNodePtr SemBinder::MakeTemporaryDtor(const ClassInfo& cls)
{
	return MakeDestructorCall(cls, false, SemNodePtr());
}

void SemBinder::AnalyzeMemberInits(const DeferredBody& body, SemNode& item)
{
	const ClassInfo& cls = *body.cls;
	std::map<string, const AstMemInitializer*> by_field;
	const AstMemInitializer* base_init = 0;
	for (size_t i = 0; i < body.decl->mem_initializers.size(); i++)
	{
		const AstMemInitializer& mem = body.decl->mem_initializers[i];
		if (mem.id.IsPlainIdentifier() &&
		    FindClassField(cls, mem.id.parts[0].identifier))
		{
			const string& name = mem.id.parts[0].identifier;
			if (by_field.count(name))
				throw runtime_error("duplicate member initializer " + name);
			by_field[name] = &mem;
			continue;
		}
		TypePtr named = ResolveTypeName(mem.id);
		if (named->kind == TK_CLASS && named->named == cls.entity)
		{
			// 12.6.2p6: a mem-initializer naming the class itself
			// delegates; no other initializer may appear.
			if (body.decl->mem_initializers.size() != 1)
				throw runtime_error("delegating constructor has other "
				                    "mem-initializers");
			vector<SemValue> values;
			const AstInitializer& init = *mem.init;
			if (init.kind == INIT_PAREN)
				for (size_t j = 0; j < init.args.size(); j++)
					values.push_back(analyzer_.Analyze(*init.args[j]));
			else if (init.kind == INIT_BRACED)
				for (size_t j = 0;
				     j < init.expr->arguments.size(); j++)
					values.push_back(analyzer_.Analyze(
						*init.expr->arguments[j]));
			int index = ResolveClassConstructor(
				cls, values, false, "delegating constructor");
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < values.size(); j++)
				arg_nodes.push_back(std::move(values[j].node));
			item.children.push_back(MakeConstructorCall(
				cls, index, false,
				AddressOfNode(ThisObjectExpr()),
				std::move(arg_nodes)));
			return;
		}
		if (cls.base && named->kind == TK_CLASS &&
		    named->named == cls.base->entity)
		{
			if (base_init)
				throw runtime_error("duplicate base initializer");
			base_init = &mem;
			continue;
		}
		throw runtime_error("member initializer names no member or base");
	}
	bf_units_written_.clear();
	vector<SemNodePtr> actions;
	if (cls.base)
	{
		if (base_init)
		{
			vector<SemValue> values;
			const AstInitializer& init = *base_init->init;
			if (init.kind == INIT_PAREN)
				for (size_t i = 0; i < init.args.size(); i++)
					values.push_back(analyzer_.Analyze(*init.args[i]));
			else if (init.kind == INIT_BRACED)
				for (size_t i = 0; i < init.expr->arguments.size(); i++)
					values.push_back(
						analyzer_.Analyze(*init.expr->arguments[i]));
			int index = ResolveClassConstructor(*cls.base, values, false,
			                                    "base initializer");
			vector<SemNodePtr> arg_nodes;
			for (size_t i = 0; i < values.size(); i++)
				arg_nodes.push_back(std::move(values[i].node));
			actions.push_back(MakeConstructorCall(*cls.base, index, true,
			                                      ThisBaseAddress(cls),
			                                      std::move(arg_nodes)));
		}
		else
			AppendBaseDefaultInit(cls, actions);
	}
	else if (base_init)
		throw runtime_error("base initializer without a base class");
	size_t matched = base_init ? 1 : 0;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		std::map<string, const AstMemInitializer*>::const_iterator found =
			by_field.find(field.name);
		if (found != by_field.end())
		{
			matched++;
			AppendMemberInit(cls, field, found->second->init.get(),
			                 actions);
		}
		else if (!cls.is_union)
			// 12.6.2p8: union variant members are not implicitly
			// initialized.
			AppendFieldDefaultInit(cls, field, actions);
	}
	if (matched != body.decl->mem_initializers.size())
		throw runtime_error("member initializer names no member");
	for (size_t i = 0; i < actions.size(); i++)
		item.children.push_back(std::move(actions[i]));
}

void SemBinder::AnalyzeDtorEpilogue(const ClassInfo& cls, SemNode& item)
{
	vector<SemNodePtr> actions;
	for (size_t i = cls.fields.size(); i-- > 0;)
	{
		const ClassField& field = cls.fields[i];
		TypePtr bare = RemoveTopCv(field.type);
		if (bare->kind == TK_ARRAY)
		{
			TypePtr element = RemoveTopCv(bare->target);
			const ClassInfo* member_cls = element->kind == TK_CLASS
				? unit_.classes.Find(element->named) : 0;
			if (!member_cls ||
			    !unit_.classes.DestructionHasEffects(*member_cls))
				continue;
			for (unsigned long long j = bare->bound; j-- > 0;)
				actions.push_back(MakeDestructorCall(
					*member_cls, false,
					AddressOfNode(SubscriptNode(ThisFieldExpr(field),
					                            j))));
			continue;
		}
		const ClassInfo* member_cls = bare->kind == TK_CLASS
			? unit_.classes.Find(bare->named) : 0;
		if (!member_cls ||
		    !unit_.classes.DestructionHasEffects(*member_cls))
			continue;
		actions.push_back(MakeDestructorCall(
			*member_cls, false, AddressOfNode(ThisFieldExpr(field))));
	}
	if (cls.base && unit_.classes.DestructionHasEffects(*cls.base))
		actions.push_back(MakeDestructorCall(*cls.base, true,
		                                     ThisBaseAddress(cls)));
	for (size_t i = 0; i < actions.size(); i++)
		item.children.push_back(std::move(actions[i]));
}

// --- implicit special members ----------------------------------------------

bool SemBinder::NodeMayThrow(const SemNode& node) const
{
	if (node.kind == SN_CALL_EXPRESSION && !node.children.empty() &&
	    node.children[0]->kind == SN_CALLEE &&
	    !node.children[0]->unwind_no)
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (NodeMayThrow(*node.children[i]))
			return true;
	return false;
}

void SemBinder::EnsureImplicitDefaultCtor(const ClassInfo& cls_in,
                                          bool out_of_class)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	if (cls.implicit_ctor_built)
		return;
	if (cls.has_user_ctor)
		throw runtime_error("no default constructor for " +
		                    cls.entity->display);
	cls.implicit_ctor_built = true;
	const string& base_name = cls.members->name;
	Scope* fn_scope =
		model_.CreateScope(SCOPE_FUNCTION, base_name, cls.members);
	TypePtr ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                     vector<TypePtr>(), false);
	DeferredBody body;
	body.name = base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = ctor_type;
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
	vector<SemNodePtr> actions;
	try
	{
		if (cls.base)
			AppendBaseDefaultInit(cls, actions);
		for (size_t i = 0; i < cls.fields.size(); i++)
			if (!cls.is_union &&
			    (!cls.fields[i].name.empty() ||
			     !cls.fields[i].is_bit_field))
				AppendFieldDefaultInit(cls, cls.fields[i], actions);
	}
	catch (...)
	{
		current_ = saved_scope;
		method_ = saved_method;
		throw;
	}
	current_ = saved_scope;
	method_ = saved_method;
	for (size_t i = 0; i < actions.size(); i++)
		node->children.push_back(std::move(actions[i]));
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	cls.implicit_ctor_unwind_no = !may_throw;
	unit_.deferred.push_back(std::move(item));
}

void SemBinder::EnsureImplicitDtor(const ClassInfo& cls_in,
                                   bool out_of_class)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	if (cls.implicit_dtor_built || cls.has_user_dtor)
		return;
	cls.implicit_dtor_built = true;
	const string& base_name = cls.members->name;
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, "~" + base_name,
	                                     cls.members);
	DeferredBody body;
	body.name = "~" + base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                      vector<TypePtr>(), false);
	SemNodePtr item = BuildFunctionNode(body, SF_DESTRUCTOR);
	SemNode* node = item.get();

	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	current_ = fn_scope;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = body.name;
	method_.this_type = node->type->parameters[0];
	try
	{
		AnalyzeDtorEpilogue(cls, *node);
	}
	catch (...)
	{
		current_ = saved_scope;
		method_ = saved_method;
		throw;
	}
	current_ = saved_scope;
	method_ = saved_method;
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	cls.implicit_dtor_unwind_no = !may_throw;
	unit_.deferred.push_back(std::move(item));
}
