#include "sema/sem_expr.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 member access and method calls: the `.` / `->` / implicit-this
// resolution paths of the expression analyzer, including the implicit
// object parameter in overload resolution and class temporaries.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

// The lookup of `name` along the object class's base chain.
const ScopeBinding* FindMemberBinding(TypesModel& model,
                                      const NamedTypeInfo* cls,
                                      const string& name)
{
	Scope* members = model.MemberScope(cls);
	if (!members)
		return 0;
	for (const Scope* link = members; link; link = link->class_base)
		if (const ScopeBinding* own = FindOwnBinding(*link, name))
			return own;
	return 0;
}

}  // namespace

// The type-name of `~T` / `p->~N::T` with the tilde dropped, for
// resolution against the scope model.
AstName SemExprAnalyzer::MakeDestructorTypeName(const AstName& name)
{
	AstName type_name;
	type_name.global_scope = name.global_scope;
	for (size_t i = 0; i + 1 < name.parts.size(); i++)
	{
		AstNamePart part;
		part.kind = NP_IDENTIFIER;
		part.identifier = name.parts[i].identifier;
		type_name.parts.push_back(std::move(part));
	}
	AstNamePart terminal;
	terminal.kind = NP_IDENTIFIER;
	terminal.identifier = name.parts.back().identifier;
	type_name.parts.push_back(std::move(terminal));
	return type_name;
}

// An explicit destructor call on a (possibly const) class object.
SemValue SemExprAnalyzer::MakeExplicitDestructorCall(SemValue object,
                                                     const ClassInfo& cls,
                                                     bool arrow)
{
	// Resolve through the binder first: deleted/access checks run and
	// an implicit destructor synthesizes its demand-emitted body.
	host_.MakeTemporaryDtor(cls);
	SemValue value;
	value.type = MakeFundamentalType(FT_VOID);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_CALL_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	TypePtr dtor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                     vector<TypePtr>(), false);
	const string& base_name = cls.members->name;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = base_name + "::~" + base_name;
	callee->type = ThisAdjustedType(cls.entity, dtor_type);
	callee->entity_scope = cls.members;
	callee->entity_name = "~" + base_name;
	callee->is_method = true;
	callee->special = SF_DESTRUCTOR;
	value.node->children.push_back(std::move(callee));
	if (arrow)
		value.node->children.push_back(std::move(object.node));
	else
		value.node->children.push_back(
			AddressOfObject(std::move(object.node)));
	return value;
}

SemNodePtr SemExprAnalyzer::ImplicitThisObject()
{
	TypePtr this_type = host_.CurrentThisType();
	SemNodePtr id = MakeSemNode(SN_ID_EXPRESSION);
	id->name = "this";
	id->type = this_type;
	id->category = VC_PRVALUE;
	id->entity_scope = host_.CurrentScope();
	// The `this` binding lives in the enclosing function scope.
	for (const Scope* scope = host_.CurrentScope(); scope;
	     scope = scope->parent)
		if (FindOwnBinding(*scope, "this"))
		{
			id->entity_scope = scope;
			break;
		}
	id->entity_name = "this";
	SemNodePtr deref = MakeSemNode(SN_UNARY_EXPRESSION);
	deref->type = this_type->target;
	deref->category = VC_LVALUE;
	deref->has_op = true;
	deref->op = OP_STAR;
	deref->op_spelling = "*";
	deref->children.push_back(std::move(id));
	return deref;
}

SemNodePtr SemExprAnalyzer::AddressOfObject(SemNodePtr object)
{
	TypePtr bare = object->type;
	if (IsReferenceType(bare))
		bare = bare->target;
	SemNodePtr address = MakeSemNode(SN_UNARY_EXPRESSION);
	address->type = MakePointerType(RemoveTopCv(bare), false, false);
	address->category = VC_PRVALUE;
	address->has_op = true;
	address->op = OP_AMP;
	address->op_spelling = "&";
	address->children.push_back(std::move(object));
	return address;
}

// `pointer->member`: the object expression is the dereferenced pointer.
// A class operand drills down through its operator-> (13.5.6).
SemValue SemExprAnalyzer::DereferenceObject(SemValue object)
{
	for (int depth = 0; object.type->kind == TK_CLASS && depth < 16;
	     depth++)
	{
		vector<SemValue> operands;
		operands.push_back(std::move(object));
		SemValue overloaded;
		if (!ResolveOperatorCall("->", operands, true, overloaded))
			throw runtime_error("no operator-> for the class operand");
		object = std::move(overloaded);
	}
	TypePtr type = object.type;
	if (type->kind == TK_ARRAY)
		throw runtime_error("arrow member access on an array");
	if (type->kind != TK_POINTER)
		throw runtime_error("arrow member access on a non-pointer");
	SemValue value;
	value.type = type->target;
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_UNARY_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_LVALUE;
	value.node->has_op = true;
	value.node->op = OP_STAR;
	value.node->op_spelling = "*";
	value.node->children.push_back(std::move(object.node));
	return value;
}

// Resolved member access over an analyzed class object expression.
SemValue SemExprAnalyzer::AnalyzeMemberAccess(SemValue object,
                                              const string& name,
                                              ETokenType op,
                                              bool implicit_this)
{
	if (object.type && object.type->kind == TK_CLASS)
		host_.RequireCompleteType(object.type->named);
	if (object.type->kind != TK_CLASS)
		throw runtime_error("member access on a non-class value");
	const NamedTypeInfo* object_entity = object.type->named;
	const ScopeBinding* member =
		FindMemberBinding(host_.Model(), object_entity, name);
	if (!member)
		throw runtime_error("no member named " + name);
	if (!host_.InClassContextOrFriend(object_entity))
		host_.CheckMemberAccess(member->home, member->access, name,
		                        object_entity);
	if (member->kind == SB_ENUMERATOR)
	{
		SemValue value;
		value.node = MakeSemNode(SN_LITERAL);
		value.node->token = RenderConstValue(member->value);
		value.node->type = member->type;
		value.node->category = VC_PRVALUE;
		value.node->has_value = true;
		value.node->value = member->value;
		value.type = member->type;
		value.category = VC_PRVALUE;
		return value;
	}
	if (member->kind == SB_FUNCTION)
	{
		SemValue value;
		value.function_set = true;
		value.overloads.push_back(member->type);
		for (size_t i = 0; i < member->overloads.size(); i++)
			value.overloads.push_back(member->overloads[i]);
		value.fn_owner = member->owner;
		value.fn_name = member->name;
		value.category = VC_LVALUE;
		value.member_class = object_entity;
		value.member_type = member->type;
		value.member_fn = member;
		value.member_object = std::move(object.node);
		value.type = member->type;
		return value;
	}
	if (member->kind != SB_VARIABLE)
		throw OutsideBoundary("non-data-member access");
	const ClassInfo* owner_cls = 0;
	if (const NamedTypeInfo* owner_entity =
	        host_.Model().ScopeEntity(member->owner))
		owner_cls = host_.Classes().Find(owner_entity);
	const ClassField* field =
		owner_cls ? FindClassField(*owner_cls, name) : 0;
	if (!field)
		// Static data member: the object designates the class only.
		return AnalyzeStaticMemberValue(*member, name);
	// 5.2.5p4: the member lvalue carries the union of the object's and
	// the member's cv-qualification; mutable members drop const.
	bool object_const = false;
	bool object_volatile = false;
	TopCv(object.type, object_const, object_volatile);
	if (member->is_mutable)
		object_const = false;
	SemValue value;
	if (IsReferenceType(field->type))
		value.type = field->type->target;
	else
		value.type = MakeCvQualifiedType(field->type, object_const,
		                                 object_volatile);
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_MEMBER_EXPRESSION);
	value.node->name = name;
	value.node->type = value.type;
	value.node->member_ref = IsReferenceType(field->type);
	value.node->category = VC_LVALUE;
	if (!implicit_this)
	{
		value.node->has_op = true;
		value.node->op = op;
	}
	value.node->member_offset = field->offset;
	// A using-imported member belongs to the importing class for
	// addressing (its base sits at offset zero either way).
	value.node->base_hops =
		BaseClassDistance(object_entity,
		                  host_.Model().ScopeEntity(member->home));
	if (value.node->base_hops < 0)
		value.node->base_hops = BaseClassDistance(
			object_entity, host_.Model().ScopeEntity(member->owner));
	value.node->is_bit_field = field->is_bit_field;
	value.node->bit_offset = field->bit_offset;
	value.node->bit_width = field->bit_width;
	value.node->entity_scope = member->owner;
	value.node->entity_name = name;
	value.node->children.push_back(std::move(object.node));
	return value;
}

// A static data member (or recorded constant) named through an object
// or a qualified name: behaves as the namespace-scope entity.
SemValue SemExprAnalyzer::AnalyzeStaticMemberValue(
	const ScopeBinding& binding, const string& written)
{
	SemValue value;
	// A member without a recorded constant demands its registered
	// out-of-class definition first: the definition may carry the
	// in-TU constant that lets this read fold (9.4.2p2 with 14.7.1p8).
	if (!binding.has_value)
		host_.OnStaticMemberReferenced(binding);
	if (binding.has_value)
	{
		// Constant static members fold like enumerators.
		value.node = MakeSemNode(SN_LITERAL);
		value.node->token = RenderConstValue(binding.value);
		value.node->type = binding.type;
		value.node->category = VC_PRVALUE;
		value.node->has_value = true;
		value.node->value = binding.value;
		value.type = RemoveTopCv(binding.type);
		value.category = VC_PRVALUE;
		return value;
	}
	TypePtr declared = binding.type;
	if (IsReferenceType(declared))
		declared = declared->target;
	value.type = declared;
	value.category = VC_LVALUE;
	value.node = MakeSemNode(SN_ID_EXPRESSION);
	value.node->name = written;
	value.node->type = declared;
	value.node->category = VC_LVALUE;
	value.node->entity_scope = binding.owner;
	value.node->entity_name = binding.name;
	return value;
}

// An unqualified id that resolved to a class member from inside a
// member function: implicit `(*this).name` for fields and bound-method
// sets, the plain entity for static members.
SemValue SemExprAnalyzer::AnalyzeImplicitMember(const ScopeBinding& binding,
                                                const string& written)
{
	const NamedTypeInfo* home_entity =
		host_.Model().ScopeEntity(binding.home);
	TypePtr this_type = host_.CurrentThisType();
	bool have_this = false;
	if (this_type)
	{
		const NamedTypeInfo* this_entity = this_type->target->named;
		have_this = BaseClassDistance(this_entity, home_entity) >= 0;
	}
	if (binding.kind == SB_VARIABLE)
	{
		const ClassInfo* owner_cls = 0;
		if (const NamedTypeInfo* owner_entity =
		        host_.Model().ScopeEntity(binding.owner))
			owner_cls = host_.Classes().Find(owner_entity);
		const ClassField* field =
			owner_cls ? FindClassField(*owner_cls, binding.name) : 0;
		if (!field)
		{
			host_.CheckMemberAccess(binding.home, binding.access,
			                        binding.name);
			return AnalyzeStaticMemberValue(binding, written);
		}
		if (!have_this)
			throw runtime_error("member " + written +
			                    " used without an object");
		// The binding already names the exact member (a qualified use
		// may reach a base field the derived class hides), so the
		// member node builds directly over *this.
		host_.CheckMemberAccess(binding.home, binding.access,
		                        binding.name);
		SemNodePtr object = ImplicitThisObject();
		const NamedTypeInfo* this_entity = this_type->target->named;
		bool object_const = false;
		bool object_volatile = false;
		TopCv(this_type->target, object_const, object_volatile);
		if (binding.is_mutable)
			object_const = false;
		SemValue value;
		if (IsReferenceType(field->type))
			value.type = field->type->target;
		else
			value.type = MakeCvQualifiedType(field->type, object_const,
			                                 object_volatile);
		value.category = VC_LVALUE;
		value.node = MakeSemNode(SN_MEMBER_EXPRESSION);
		value.node->name = binding.name;
		value.node->type = value.type;
		value.node->member_ref = IsReferenceType(field->type);
		value.node->category = VC_LVALUE;
		value.node->member_offset = field->offset;
		value.node->base_hops = BaseClassDistance(
			this_entity, host_.Model().ScopeEntity(binding.owner));
		value.node->is_bit_field = field->is_bit_field;
		value.node->bit_offset = field->bit_offset;
		value.node->bit_width = field->bit_width;
		value.node->entity_scope = binding.owner;
		value.node->entity_name = binding.name;
		value.node->children.push_back(std::move(object));
		return value;
	}
	if (binding.kind != SB_FUNCTION)
		throw runtime_error(written + " does not name a value");
	host_.CheckMemberAccess(binding.home, binding.access, binding.name);
	SemValue value;
	value.function_set = true;
	value.overloads.push_back(binding.type);
	for (size_t i = 0; i < binding.overloads.size(); i++)
		value.overloads.push_back(binding.overloads[i]);
	value.fn_owner = binding.owner;
	value.fn_name = binding.name;
	value.category = VC_LVALUE;
	value.member_class = home_entity;
	value.member_type = binding.type;
	value.member_fn = &binding;
	if (have_this)
		value.member_object = ImplicitThisObject();
	value.type = binding.type;
	return value;
}

// A class temporary constructed from a functional cast: `T(args)` over
// the class subset materializes a temporary object and runs the
// selected constructor. `braced_assign` marks the braced-init-list
// assignment RHS, the one temporary whose effect-free destruction the
// reference output elides.
SemValue SemExprAnalyzer::MakeTemporaryObject(
	const TypePtr& class_type, const vector<AstExprPtr>& arguments,
	bool braced_assign)
{
	if (class_type->kind == TK_CLASS)
		host_.RequireCompleteType(class_type->named);
	const ClassInfo* cls = host_.Classes().Find(class_type->named);
	if (!cls || !class_type->named->complete)
		throw runtime_error("temporary of an incomplete class");
	vector<SemValue> args;
	for (size_t i = 0; i < arguments.size(); i++)
		args.push_back(Analyze(*arguments[i]));
	if (cls->is_aggregate && !cls->has_user_ctor && !args.empty())
	{
		// 5.2.3p3/8.5.1: a braced temporary of an aggregate class
		// initializes field-wise.
		SemNodePtr action =
			host_.MakeAggregateTemporary(*cls, std::move(args));
		SemValue value;
		value.type = RemoveTopCv(class_type);
		value.category = VC_PRVALUE;
		value.node = std::move(action);
		return value;
	}
	int winner = host_.ResolveClassCtorHost(*cls, args, false, "temporary");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < args.size(); i++)
		arg_nodes.push_back(std::move(args[i].node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, winner, false, SemNodePtr(), std::move(arg_nodes));
	// 5.2.3: the functional-cast temporary's construction is explicit
	// even when the implicit default constructor does nothing.
	action->trivial_init = false;
	action->type = RemoveTopCv(class_type);
	action->category = VC_PRVALUE;
	if (host_.Classes().NeedsDestruction(*cls))
	{
		// 12.2: the temporary is destroyed at the end of the enclosing
		// full expression, even when the chain is effect-free; only
		// the braced-assignment RHS elides an effect-free cleanup
		// (the resolved destructor is still access-checked).
		if (!braced_assign || host_.Classes().DestructionHasEffects(*cls))
		{
			action->needs_dtor = true;
			action->children.push_back(host_.MakeTemporaryDtor(*cls));
		}
		else
			host_.MakeTemporaryDtor(*cls);
	}
	SemValue value;
	value.type = RemoveTopCv(class_type);
	value.category = VC_PRVALUE;
	value.node = std::move(action);
	return value;
}

// `object.name(args)` / `pointer->name(args)`: data members holding
// callables call indirectly through the member value; member functions
// resolve against the implicit object argument.
SemValue SemExprAnalyzer::AnalyzeMemberCall(const AstExpr& expr,
                                            const AstExpr& callee)
{
	SemValue object = Analyze(*callee.operands[0]);
	if (!callee.name.parts.empty() && callee.name.parts.back().tilde)
	{
		// 5.2.4 pseudo-destructor / 12.4p15 explicit destructor call.
		if (!expr.arguments.empty())
			throw runtime_error("destructor call takes no arguments");
		if (callee.op == OP_ARROW && object.type->kind != TK_POINTER)
			throw runtime_error("arrow destructor call on a non-pointer");
		TypePtr named = host_.TryResolveCalleeType(
			MakeDestructorTypeName(callee.name));
		TypePtr object_type = callee.op == OP_ARROW
			? object.type->target : object.type;
		if (!named ||
		    !TypeEquals(RemoveTopCv(named), RemoveTopCv(object_type)))
			throw runtime_error("destructor name does not match the "
			                    "object type");
		if (RemoveTopCv(object_type)->kind == TK_CLASS)
		{
			const ClassInfo* cls = host_.Classes().Find(
				RemoveTopCv(object_type)->named);
			if (cls && host_.Classes().NeedsDestruction(*cls))
				return MakeExplicitDestructorCall(std::move(object),
				                                  *cls,
				                                  callee.op == OP_ARROW);
		}
		// A scalar pseudo-destructor (or trivial class destructor)
		// evaluates its object expression and yields void.
		SemValue value;
		value.type = MakeFundamentalType(FT_VOID);
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_CAST_EXPRESSION);
		value.node->type = value.type;
		value.node->category = VC_PRVALUE;
		value.node->children.push_back(std::move(object.node));
		return value;
	}
	if (callee.op == OP_ARROW)
		object = DereferenceObject(std::move(object));
	string name;
	bool qualified = false;
	const AstNamePart& last = callee.name.parts.back();
	if (callee.name.IsPlainIdentifier())
		name = callee.name.parts[0].identifier;
	else if (callee.name.parts.size() == 1 &&
	         last.kind == NP_OPERATOR_FUNCTION)
		name = "operator " + last.operator_text;
	else if (callee.name.parts.size() == 1 &&
	         last.kind == NP_CONVERSION_FUNCTION)
		// An explicit conversion call: the type-id resolves to the
		// canonical member name.
		name = "operator " + DescribeType(
			host_.ResolveCastTypeId(*last.conversion_type));
	else if (last.kind == NP_IDENTIFIER)
	{
		// PA17 10.3p15: explicit scope qualification (`d.Base::f()`)
		// names the function to call; the call is direct, without
		// virtual dispatch.
		name = last.identifier;
		qualified = true;
	}
	else
		throw OutsideBoundary("member name form");
	if (object.type->kind != TK_CLASS)
		throw runtime_error("member call on a non-class value");
	const NamedTypeInfo* lookup_entity = object.type->named;
	if (qualified)
		lookup_entity = ResolveMemberQualifier(callee.name,
		                                       object.type->named);
	const ScopeBinding* member =
		FindMemberBinding(host_.Model(), lookup_entity, name);
	if (!member)
		throw runtime_error("no member named " + name);
	if (member->kind == SB_VARIABLE)
	{
		// A data member holding a function pointer: the call is
		// indirect through the member value.
		SemValue fn = AnalyzeMemberAccess(std::move(object), name,
		                                  callee.op, false);
		if (fn.type->kind == TK_CLASS)
			return AnalyzeFunctorCall(std::move(fn), expr);
		TypePtr function_type;
		if (fn.type->kind == TK_POINTER &&
		    fn.type->target->kind == TK_FUNCTION)
			function_type = fn.type->target;
		else if (fn.type->kind == TK_FUNCTION)
			function_type = fn.type;
		else
			throw runtime_error("member is not callable");
		vector<SemValue> args;
		for (size_t i = 0; i < expr.arguments.size(); i++)
			args.push_back(Analyze(*expr.arguments[i]));
		CheckCallArguments(function_type, args);
		SemValue value = CallResult(function_type);
		value.node->children.push_back(std::move(fn.node));
		for (size_t i = 0; i < args.size(); i++)
			value.node->children.push_back(std::move(args[i].node));
		return value;
	}
	if (member->kind != SB_FUNCTION)
		throw runtime_error(name + " is not a member function");
	if (!host_.InClassContextOrFriend(object.type->named))
		host_.CheckMemberAccess(member->home, member->access, name,
		                        object.type->named);
	return AnalyzeMethodCall(std::move(object), *member, expr.arguments,
	                         qualified);
}

// The class named by the qualifier of a qualified member-call name
// (`d.Base::f()`): it must resolve to the object's class or one of its
// bases. The qualifier resolves against the enclosing scope (the
// class-scope leg of 3.4.5p4 stays outside the subset and fails here).
const NamedTypeInfo* SemExprAnalyzer::ResolveMemberQualifier(
	const AstName& name, const NamedTypeInfo* object_entity)
{
	AstName prefix;
	prefix.global_scope = name.global_scope;
	for (size_t i = 0; i + 1 < name.parts.size(); i++)
	{
		if (name.parts[i].kind != NP_IDENTIFIER || name.parts[i].tilde)
			throw OutsideBoundary("member name form");
		AstNamePart part;
		part.kind = NP_IDENTIFIER;
		part.identifier = name.parts[i].identifier;
		prefix.parts.push_back(std::move(part));
	}
	TypePtr named = host_.TryResolveCalleeType(prefix);
	if (!named || RemoveTopCv(named)->kind != TK_CLASS)
		throw runtime_error("member qualifier does not name a class");
	const NamedTypeInfo* entity = RemoveTopCv(named)->named;
	if (BaseClassDistance(object_entity, entity) < 0)
		throw runtime_error("member qualifier does not name the "
		                    "object's class or a base class");
	return entity;
}

// Overload resolution of a member function call with an implicit
// object argument (13.3.1p2-p4): non-static overloads bind the object
// to a reference to the method-cv-qualified class; static overloads
// take a neutral identity binding.
SemValue SemExprAnalyzer::AnalyzeMethodCall(
	SemValue object, const ScopeBinding& binding,
	const vector<AstExprPtr>& arguments, bool qualified)
{
	const NamedTypeInfo* object_entity = object.type->named;
	vector<TypePtr> declared;
	declared.push_back(binding.type);
	for (size_t i = 0; i < binding.overloads.size(); i++)
		declared.push_back(binding.overloads[i]);

	vector<SemValue> args;
	vector<ConversionSource> sources;
	ConversionSource object_source;
	object_source.type = object.type;
	object_source.category = object.category == VC_PRVALUE
		? VC_XVALUE : object.category;
	sources.push_back(object_source);
	for (size_t i = 0; i < arguments.size(); i++)
	{
		args.push_back(Analyze(*arguments[i]));
		sources.push_back(MakeConversionSource(args.back()));
	}
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	const NamedTypeInfo* owner_entity =
		host_.Model().ScopeEntity(binding.owner);
	for (size_t c = 0; c < declared.size(); c++)
	{
		const TypePtr& fn = declared[c];
		bool is_static = c < binding.fn_static.size() &&
			binding.fn_static[c];
		TypePtr object_param;
		if (is_static)
			// 13.3.1p4: the implicit object argument matches anything;
			// an identity binding keeps the comparison neutral.
			object_param = MakeReferenceType(RemoveTopCv(object.type),
			                                 false, true);
		else
		{
			// 13.3.1p4 with 8.3.5p6: the implicit object parameter
			// carries the ref-qualifier; without one it binds either
			// value category.
			TypePtr class_type = MakeNamedType(
				TK_CLASS, owner_entity ? owner_entity : object_entity);
			class_type = MakeCvQualifiedType(class_type, fn->is_const,
			                                 fn->is_volatile);
			bool rvalue_param = fn->ref_qual == 2 ||
				(fn->ref_qual == 0 && object.category != VC_LVALUE);
			object_param = MakeReferenceType(class_type, rvalue_param,
			                                 true);
		}
		vector<TypePtr> parameters;
		parameters.push_back(object_param);
		for (size_t i = 0; i < fn->parameters.size(); i++)
			parameters.push_back(fn->parameters[i]);
		candidates.push_back(MakeFunctionType(fn->target, parameters,
		                                      fn->variadic));
		size_t required = fn->parameters.size();
		const vector<const AstExpr*>* defaults =
			c < binding.fn_defaults.size() ? &binding.fn_defaults[c] : 0;
		while (defaults && required > 0 && required <= defaults->size()
		       && (*defaults)[required - 1])
			required--;
		min_arity.push_back(required + 1);
	}
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(candidates, sources, conversions,
	                                   &min_arity);
	if (winner < binding.fn_deleted.size() && binding.fn_deleted[winner])
		throw runtime_error("use of deleted member function");
	EMemberAccess access = winner < binding.fn_access.size()
		? binding.fn_access[winner] : MA_PUBLIC;
	host_.CheckMemberAccess(binding.home, access, binding.name,
	                        object_entity);
	// A using-declaration merges overloads from another class; the
	// winner keeps its real declaring scope for identity/addressing.
	const Scope* owner_scope = binding.owner;
	if (winner < binding.fn_owner.size() && binding.fn_owner[winner])
		owner_scope = binding.fn_owner[winner];
	// PA16: a qualified or explicit call can select an implicitly
	// declared assignment operator; synthesize it on first selection.
	if (binding.name == "operator =")
		if (const NamedTypeInfo* assign_entity =
		        host_.Model().ScopeEntity(owner_scope))
			host_.EnsureAssignSpecial(assign_entity, winner);
	bool is_static = winner < binding.fn_static.size() &&
		binding.fn_static[winner];
	const TypePtr& fn = declared[winner];
	for (size_t i = 0; i < args.size(); i++)
		if (i < fn->parameters.size())
			ApplyConversion(args[i], conversions[i + 1],
			                fn->parameters[i]);
	for (size_t i = args.size(); i < fn->parameters.size(); i++)
	{
		SemValue filled = Analyze(*binding.fn_defaults[winner][i]);
		CopyInitialize(filled, fn->parameters[i], "default argument");
		args.push_back(std::move(filled));
	}

	SemValue value = CallResult(fn);
	const NamedTypeInfo* winner_entity =
		host_.Model().ScopeEntity(owner_scope);
	const NamedTypeInfo* callee_class =
		winner_entity ? winner_entity : object_entity;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(owner_scope, binding.name);
	callee->type = is_static ? fn
	                         : ThisAdjustedType(callee_class, fn);
	callee->entity_scope = owner_scope;
	callee->entity_name = binding.name;
	callee->is_method = !is_static;
	if (winner < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[winner])
		callee->unwind_no = true;
	// PA17 dynamic dispatch: an unqualified call to a virtual member
	// dispatches through the static class's vtable slot (10.3p6); the
	// dynamic callee may not be the statically selected one.
	if (!is_static && !qualified)
	{
		const ClassInfo* static_cls = host_.Classes().Find(object_entity);
		if (static_cls)
		{
			// 15.4p3: overriders keep a no-less-strict exception
			// specification, so the static callee's unwind fact holds
			// for every dynamic target.
			int slot = FindVirtualSlotIndex(*static_cls, binding.name, fn);
			if (slot >= 0)
				callee->vtable_slot = slot;
		}
	}
	value.node->children.push_back(std::move(callee));
	if (!is_static)
	{
		// An inherited method receives the base subobject's address; a
		// using-imported one belongs to the importing class for
		// addressing (its base sits at offset zero either way).
		int hops = binding.home != owner_scope
			? 0 : BaseClassDistance(object_entity, callee_class);
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = MakeNamedType(TK_CLASS, callee_class);
			adjusted->category = VC_LVALUE;
			adjusted->base_hops = hops;
			adjusted->children.push_back(std::move(object.node));
			object.node = std::move(adjusted);
		}
		value.node->children.push_back(
			AddressOfObject(std::move(object.node)));
	}
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// A member function call without any object context: only static
// overloads participate (9.3.1p3).
SemValue SemExprAnalyzer::AnalyzeStaticMethodCall(
	const AstExpr& expr, const ScopeBinding& binding)
{
	vector<TypePtr> declared;
	declared.push_back(binding.type);
	for (size_t i = 0; i < binding.overloads.size(); i++)
		declared.push_back(binding.overloads[i]);
	vector<TypePtr> candidates;
	vector<size_t> positions;
	vector<size_t> min_arity;
	for (size_t c = 0; c < declared.size(); c++)
	{
		if (c >= binding.fn_static.size() || !binding.fn_static[c])
			continue;
		candidates.push_back(declared[c]);
		positions.push_back(c);
		size_t required = declared[c]->parameters.size();
		const vector<const AstExpr*>* defaults =
			c < binding.fn_defaults.size() ? &binding.fn_defaults[c] : 0;
		while (defaults && required > 0 && required <= defaults->size()
		       && (*defaults)[required - 1])
			required--;
		min_arity.push_back(required);
	}
	if (candidates.empty())
		throw runtime_error("member function " + binding.name +
		                    " called without an object");
	vector<SemValue> args;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < expr.arguments.size(); i++)
	{
		args.push_back(Analyze(*expr.arguments[i]));
		sources.push_back(MakeConversionSource(args.back()));
	}
	vector<ImplicitConversion> conversions;
	size_t best = SelectBestOverload(candidates, sources, conversions,
	                                 &min_arity);
	size_t winner = positions[best];
	if (winner < binding.fn_deleted.size() && binding.fn_deleted[winner])
		throw runtime_error("use of deleted member function");
	EMemberAccess access = winner < binding.fn_access.size()
		? binding.fn_access[winner] : MA_PUBLIC;
	host_.CheckMemberAccess(binding.home, access, binding.name);
	const TypePtr& fn = declared[winner];
	for (size_t i = 0; i < args.size(); i++)
		if (i < fn->parameters.size())
			ApplyConversion(args[i], conversions[i],
			                fn->parameters[i]);
	for (size_t i = args.size(); i < fn->parameters.size(); i++)
	{
		SemValue filled = Analyze(*binding.fn_defaults[winner][i]);
		CopyInitialize(filled, fn->parameters[i], "default argument");
		args.push_back(std::move(filled));
	}
	SemValue value = CallResult(fn);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding.owner, binding.name);
	callee->type = fn;
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	if (winner < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[winner])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

// 2.14.8: a user-defined string literal calls the literal operator of
// its suffix with the character array and its length.
SemValue SemExprAnalyzer::AnalyzeStringUdl(const AstExpr& expr)
{
	const string op_name = "operator \"\"" + expr.literal_suffix;
	const ScopeBinding* binding =
		UnqualifiedLookup(host_.CurrentScope(), op_name, SLF_ANY);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("no literal operator for suffix " +
		                    expr.literal_suffix);
	SemValue text;
	text.node = MakeSemNode(SN_LITERAL);
	text.node->token = expr.literal;
	text.type = MakeArrayType(
		MakeCvQualifiedType(MakeFundamentalType(expr.literal_type),
		                    true, false),
		true, expr.literal_elements);
	text.category = VC_LVALUE;
	text.node->type = text.type;
	text.node->category = VC_LVALUE;
	text.node->is_string_literal = true;
	text.node->string_bytes = expr.literal_data;
	SemValue length;
	length.node = MakeSemNode(SN_LITERAL);
	length.node->token = std::to_string(expr.literal_elements - 1);
	length.node->type = MakeFundamentalType(FT_INT);
	length.node->category = VC_PRVALUE;
	length.node->has_value = true;
	length.node->value = ConstValue(FT_INT, expr.literal_elements - 1);
	length.type = length.node->type;

	vector<TypePtr> candidates;
	candidates.push_back(binding->type);
	for (size_t i = 0; i < binding->overloads.size(); i++)
		candidates.push_back(binding->overloads[i]);
	vector<SemValue> args;
	args.push_back(std::move(text));
	args.push_back(std::move(length));
	vector<ConversionSource> sources;
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(candidates, sources, conversions);
	const TypePtr& fn = candidates[winner];
	for (size_t i = 0; i < args.size(); i++)
		ApplyConversion(args[i], conversions[i], fn->parameters[i]);

	SemValue value = CallResult(fn);
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding->owner, binding->name);
	callee->type = fn;
	callee->entity_scope = binding->owner;
	callee->entity_name = binding->name;
	if (winner < binding->fn_unwind_no.size() &&
	    binding->fn_unwind_no[winner])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}
