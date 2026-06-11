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
	SemNodePtr address = MakeSemNode(SN_UNARY_EXPRESSION);
	address->type = MakePointerType(RemoveTopCv(object->type), false,
	                                false);
	address->category = VC_PRVALUE;
	address->has_op = true;
	address->op = OP_AMP;
	address->op_spelling = "&";
	address->children.push_back(std::move(object));
	return address;
}

// `pointer->member`: the object expression is the dereferenced pointer.
SemValue SemExprAnalyzer::DereferenceObject(SemValue object)
{
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
	if (object.type->kind != TK_CLASS)
		throw runtime_error("member access on a non-class value");
	const NamedTypeInfo* object_entity = object.type->named;
	const ScopeBinding* member =
		FindMemberBinding(host_.Model(), object_entity, name);
	if (!member)
		throw runtime_error("no member named " + name);
	host_.CheckMemberAccess(member->home, member->access, name);
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
	value.node->base_hops =
		BaseClassDistance(object_entity,
		                  host_.Model().ScopeEntity(member->owner));
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
		SemValue object;
		object.node = ImplicitThisObject();
		object.type = object.node->type;
		object.category = VC_LVALUE;
		return AnalyzeMemberAccess(std::move(object), binding.name,
		                           OP_DOT, true);
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
// selected constructor.
SemValue SemExprAnalyzer::MakeTemporaryObject(
	const TypePtr& class_type, const vector<AstExprPtr>& arguments)
{
	const ClassInfo* cls = host_.Classes().Find(class_type->named);
	if (!cls || !class_type->named->complete)
		throw runtime_error("temporary of an incomplete class");
	vector<SemValue> args;
	for (size_t i = 0; i < arguments.size(); i++)
		args.push_back(Analyze(*arguments[i]));
	int winner = -1;
	if (!cls->ctors.empty())
	{
		vector<TypePtr> candidates;
		vector<size_t> min_arity;
		for (size_t i = 0; i < cls->ctors.size(); i++)
		{
			candidates.push_back(cls->ctors[i].type);
			size_t required = cls->ctors[i].type->parameters.size();
			const vector<const AstExpr*>& defaults =
				cls->ctors[i].defaults;
			while (required > 0 && required <= defaults.size() &&
			       defaults[required - 1])
				required--;
			min_arity.push_back(required);
		}
		vector<ConversionSource> sources;
		for (size_t i = 0; i < args.size(); i++)
			sources.push_back(MakeConversionSource(args[i]));
		vector<ImplicitConversion> conversions;
		winner = (int)SelectBestOverload(candidates, sources,
		                                 conversions, &min_arity);
		if (cls->ctors[winner].deleted)
			throw runtime_error("use of deleted constructor");
		host_.CheckMemberAccess(cls->members, cls->ctors[winner].access,
		                        "constructor");
		const TypePtr& fn = candidates[winner];
		for (size_t i = 0; i < args.size(); i++)
			if (i < fn->parameters.size())
				ApplyConversion(args[i], conversions[i],
				                fn->parameters[i]);
		for (size_t i = args.size(); i < fn->parameters.size(); i++)
		{
			SemValue filled = Analyze(*cls->ctors[winner].defaults[i]);
			CopyInitialize(filled, fn->parameters[i],
			               "default argument");
			args.push_back(std::move(filled));
		}
		if (cls->ctors[winner].defaulted && fn->parameters.empty())
			winner = -1;
	}
	else if (!args.empty())
		throw runtime_error("no matching constructor for temporary");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < args.size(); i++)
		arg_nodes.push_back(std::move(args[i].node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, winner, false, SemNodePtr(), std::move(arg_nodes));
	action->type = RemoveTopCv(class_type);
	action->category = VC_PRVALUE;
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
	if (callee.op == OP_ARROW)
		object = DereferenceObject(std::move(object));
	if (!callee.name.IsPlainIdentifier())
		throw OutsideBoundary("member name form");
	const string& name = callee.name.parts[0].identifier;
	if (object.type->kind != TK_CLASS)
		throw runtime_error("member call on a non-class value");
	const ScopeBinding* member =
		FindMemberBinding(host_.Model(), object.type->named, name);
	if (!member)
		throw runtime_error("no member named " + name);
	if (member->kind == SB_VARIABLE)
	{
		// A data member holding a function pointer: the call is
		// indirect through the member value.
		SemValue fn = AnalyzeMemberAccess(std::move(object), name,
		                                  callee.op, false);
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
	host_.CheckMemberAccess(member->home, member->access, name);
	return AnalyzeMethodCall(std::move(object), *member, expr.arguments);
}

// Overload resolution of a member function call with an implicit
// object argument (13.3.1p2-p4): non-static overloads bind the object
// to a reference to the method-cv-qualified class; static overloads
// take a neutral identity binding.
SemValue SemExprAnalyzer::AnalyzeMethodCall(
	SemValue object, const ScopeBinding& binding,
	const vector<AstExprPtr>& arguments)
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
	object_source.category = VC_LVALUE;
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
			TypePtr class_type = MakeNamedType(
				TK_CLASS, owner_entity ? owner_entity : object_entity);
			class_type = MakeCvQualifiedType(class_type, fn->is_const,
			                                 fn->is_volatile);
			object_param = MakeReferenceType(class_type, false, true);
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
	host_.CheckMemberAccess(binding.home, access, binding.name);
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
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding.owner, binding.name);
	callee->type = is_static ? fn
	                         : ThisAdjustedType(object_entity, fn);
	callee->entity_scope = binding.owner;
	callee->entity_name = binding.name;
	callee->is_method = !is_static;
	if (winner < binding.fn_unwind_no.size() &&
	    binding.fn_unwind_no[winner])
		callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	if (!is_static)
		value.node->children.push_back(
			AddressOfObject(std::move(object.node)));
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
