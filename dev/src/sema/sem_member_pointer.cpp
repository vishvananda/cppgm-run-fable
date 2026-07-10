#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/const_expr.h"

using std::runtime_error;

// PA26 member-pointer semantics: `.*` / `->*` application over the
// non-virtual object model, member-pointer non-type template
// arguments, and the folded uses of member-pointer parameters.

// 5.5: `object .* pm` / `pointer ->* pm` over the non-virtual object
// model. The object adjusts to the member pointer's class subobject; a
// data access yields the member lvalue, a function access a call-only
// bound value carrying the pointed-to function type.
SemValue SemExprAnalyzer::AnalyzeMemberPointerBinary(const AstExpr& expr,
                                                     SemValue& lhs,
                                                     SemValue& rhs)
{
	TypePtr pm = RemoveTopCv(rhs.type);
	if (pm->kind != TK_MEMBER_POINTER)
		throw runtime_error("right operand is not a member pointer");
	SemValue object = std::move(lhs);
	if (expr.op == OP_ARROWSTAR)
	{
		TypePtr pointer = object.type;
		if (pointer->kind != TK_POINTER ||
		    RemoveTopCv(pointer->target)->kind != TK_CLASS)
			throw runtime_error("->* left operand is not a pointer to "
			                    "class");
		SemNodePtr deref = MakeSemNode(SN_UNARY_EXPRESSION);
		deref->type = pointer->target;
		deref->category = VC_LVALUE;
		deref->has_op = true;
		deref->op = OP_STAR;
		deref->op_spelling = "*";
		deref->children.push_back(std::move(object.node));
		object.node = std::move(deref);
		object.type = pointer->target;
		object.category = VC_LVALUE;
	}
	TypePtr object_class = RemoveTopCv(object.type);
	if (object_class->kind != TK_CLASS)
		throw runtime_error(".* left operand is not a class object");
	int hops = 0;
	unsigned long long base_offset = 0;
	EBasePath path = BaseSubobjectPath(object_class->named, pm->named,
	                                   hops, base_offset);
	if (path == BP_AMBIGUOUS)
		throw runtime_error("ambiguous base class subobject");
	if (path != BP_UNIQUE)
		throw runtime_error("object class is unrelated to the member "
		                    "pointer class");
	if (hops > 0)
	{
		SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
		adjusted->type = MakeNamedType(TK_CLASS, pm->named);
		adjusted->category = object.category == VC_PRVALUE
			? VC_XVALUE : object.category;
		adjusted->base_hops = hops;
		adjusted->base_offset = base_offset;
		adjusted->children.push_back(std::move(object.node));
		object.node = std::move(adjusted);
	}
	SemValue value;
	if (pm->target->kind == TK_FUNCTION)
	{
		// The bound value is legal only as a call target; the call
		// lowers through the pointed-to function type.
		value.type = pm->target;
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_MEMBER_POINTER_ACCESS);
		value.node->type = pm->target;
		value.node->category = VC_PRVALUE;
		value.node->children.push_back(std::move(object.node));
		value.node->children.push_back(std::move(rhs.node));
		return value;
	}
	// 5.5p5-p6: the data member lvalue carries the union of the
	// object's and the member's cv-qualification.
	bool object_const = false;
	bool object_volatile = false;
	TopCv(object.type, object_const, object_volatile);
	value.type = MakeCvQualifiedType(pm->target, object_const,
	                                 object_volatile);
	value.category = object.category == VC_PRVALUE ? VC_XVALUE
	                                               : object.category;
	value.node = MakeSemNode(SN_MEMBER_POINTER_ACCESS);
	value.node->type = value.type;
	value.node->category = value.category;
	value.node->children.push_back(std::move(object.node));
	value.node->children.push_back(std::move(rhs.node));
	return value;
}

// PA26 14.1p4: a member-function pointer template parameter's use
// re-forms the `&C::f` constant over the argument's member entity (the
// lowering renders the member's address pair from the id node).
SemValue SemExprAnalyzer::MemberPointerParamConstant(
	const ScopeBinding& binding, const TypePtr& pm)
{
	const Scope* members = binding.owner;
	const ScopeBinding* entity =
		FindOwnBinding(*members, binding.pack_element_name);
	if (!entity || entity->kind != SB_FUNCTION)
		throw runtime_error("member pointer parameter entity missing");
	SemNodePtr member = MakeSemNode(SN_ID_EXPRESSION);
	member->name = binding.pack_element_name;
	member->type = ThisAdjustedType(members->entity, pm->target);
	member->category = VC_LVALUE;
	member->entity_scope = members;
	member->entity_name = binding.pack_element_name;
	SemValue value;
	value.type = pm;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_UNARY_EXPRESSION);
	value.node->type = pm;
	value.node->category = VC_PRVALUE;
	value.node->has_op = true;
	value.node->op = OP_AMP;
	value.node->op_spelling = "&";
	value.node->children.push_back(std::move(member));
	return value;
}

// PA26 14.3.2p6: a member pointer argument spells `&C::member` (the
// declared member's type must match the parameter's member type) or a
// null pointer constant. A data member carries its typed value (the
// field offset + 1; 0 is null); the named entity carries the mangling
// and instantiation identity.
void SemBinder::ResolveMemberPointerArgument(const AstExpr* expr,
                                             const TypePtr& param_type,
                                             TemplateArg& arg)
{
	// A plain name forwarding another member pointer parameter reuses
	// the bound argument's value and entity.
	if (expr && expr->kind == EK_ID)
	{
		const ScopeBinding* alias = ResolveTerminal(expr->name, SLF_ANY);
		if (alias && alias->kind == SB_VARIABLE && alias->no_object)
		{
			if (alias->param_index >= 0)
			{
				arg.value_param = alias->param_index;
				return;
			}
			if (alias->has_value)
			{
				arg.value_type = alias->value.type;
				arg.value_bits = alias->value.bits;
				if (alias->owner &&
				    alias->owner->kind == SCOPE_CLASS)
				{
					arg.entity_scope = alias->owner;
					arg.entity_name = alias->pack_element_name;
				}
				return;
			}
		}
		if (alias && alias->kind == SB_VARIABLE && !alias->has_value &&
		    alias->owner && alias->owner->kind == SCOPE_CLASS &&
		    !alias->pack_element_name.empty())
		{
			arg.entity_scope = alias->owner;
			arg.entity_name = alias->pack_element_name;
			return;
		}
	}
	if (expr)
	{
		// A null pointer constant forms the null member pointer.
		try
		{
			ConstValue value = EvaluateConstExpr(*expr, *this);
			if (value.bits == 0)
			{
				arg.value_type = FT_LONG_LONG_INT;
				arg.value_bits = 0;
				return;
			}
		}
		catch (const std::exception&)
		{
		}
	}
	const AstName* entity = 0;
	if (expr && expr->kind == EK_UNARY && expr->op_spelling == "&" &&
	    !expr->operands.empty() && expr->operands[0]->kind == EK_ID)
		entity = &expr->operands[0]->name;
	if (!entity)
		throw runtime_error("member pointer argument does not name a "
		                    "member");
	const ScopeBinding* found = ResolveTerminal(*entity, SLF_ANY);
	if (!found || !found->home || found->home->kind != SCOPE_CLASS)
		throw runtime_error("member pointer argument does not name a "
		                    "class member");
	const NamedTypeInfo* owner_entity =
		found->owner ? found->owner->entity : 0;
	if (!owner_entity ||
	    BaseClassDistance(param_type->named, owner_entity) < 0)
		throw runtime_error("member pointer argument class mismatch");
	if (found->kind == SB_FUNCTION)
	{
		bool matched = found->type &&
			TypeEquals(found->type, param_type->target);
		for (size_t i = 0; !matched && i < found->overloads.size(); i++)
			matched = TypeEquals(found->overloads[i],
			                     param_type->target);
		// 14.8.2.2: a member function template deduces against the
		// parameter's member type.
		if (!matched)
			for (size_t t = 0; t < found->fn_templates.size(); t++)
				if (const FunctionSpecialization* spec =
				        DeduceFunctionTemplateFromTarget(
				            *found->fn_templates[t],
				            param_type->target))
				{
					OnSpecializationOdrUsed(spec);
					arg.entity_scope = spec->self.owner;
					arg.entity_name = spec->name;
					arg.entity_fn_spec = spec;
					return;
				}
		if (!matched)
			throw runtime_error("member pointer argument type mismatch");
		arg.entity_scope = found->owner;
		arg.entity_name = found->name;
		return;
	}
	if (found->kind != SB_VARIABLE)
		throw runtime_error("member pointer argument does not name a "
		                    "member");
	const ClassInfo* record = owner_entity->class_record;
	const ClassField* field =
		record ? FindClassField(*record, found->name) : 0;
	if (!field)
		throw runtime_error("member pointer argument names no field");
	if (!TypeEquals(RemoveTopCv(field->type),
	                RemoveTopCv(param_type->target)))
		throw runtime_error("member pointer argument type mismatch");
	arg.entity_scope = found->owner;
	arg.entity_name = found->name;
	arg.value_type = FT_LONG_LONG_INT;
	arg.value_bits = field->offset + 1;
}
