#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 member-function-body machinery, split from sem_class.cpp: the
// deferred in-class bodies (analyzed once the outermost class
// completes, 9.2p2), the shared function-node builder, and the
// per-body analysis context.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

}  // namespace

// --- member function bodies ---------------------------------------------

void SemBinder::BindMemberFunctionBody(const AstDecl& decl,
                                       const DeclaratorInfo& composed,
                                       const string& name)
{
	Scope* fn_scope = current_;
	Scope* declaring = fn_scope->parent;
	const NamedTypeInfo* entity = model_.ScopeEntity(declaring);
	ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
	if (!cls)
		throw runtime_error("member function of an unknown class");
	DeferredBody body;
	body.decl = &decl;
	body.composed = composed;
	body.name = name;
	body.fn_scope = fn_scope;
	body.declaring = declaring;
	body.cls = cls;
	const ScopeBinding* binding = FindOwnBinding(*declaring, name);
	if (binding)
	{
		size_t index = 0;
		for (size_t i = 0; i < binding->overloads.size(); i++)
			if (TypeEquals(binding->overloads[i], composed.type))
				index = i + 1;
		if (index < binding->fn_static.size())
			body.is_static = binding->fn_static[index];
	}
	if (OpenClass() && OpenClass()->members == declaring)
	{
		deferred_bodies_.push_back(body);
		return;
	}
	// An out-of-class member definition: the class is complete, so the
	// body analyzes immediately (and emits as a strong definition).
	body.out_of_class = true;
	// PA17: defining the class's key function anchors its vtable in
	// this translation unit (emitted strong by the lowering). An
	// instantiated member definition anchors nothing: specialization
	// vtables stay weak.
	if (!instantiating_ && cls->is_polymorphic && !cls->key_is_dtor &&
	    !cls->key_name.empty() && cls->key_name == name &&
	    TypeEquals(cls->key_type, composed.type))
		cls->key_defined_in_tu = true;
	AnalyzeDeferredBody(body);
}

void SemBinder::FlushDeferredBodies()
{
	while (!deferred_bodies_.empty())
	{
		vector<DeferredBody> batch;
		batch.swap(deferred_bodies_);
		for (size_t i = 0; i < batch.size(); i++)
		{
			if (!instantiating_)
			{
				AnalyzeDeferredBody(batch[i]);
				continue;
			}
			// 14.7.1: an instantiated member body is ill-formed only
			// if a use requires it. Bodies bind eagerly here, so an
			// ill-formed one poisons its weak definition instead of
			// the whole instantiation; a demand for it still fails.
			try
			{
				AnalyzeDeferredBody(batch[i]);
			}
			catch (const std::exception& error)
			{
				AppendPoisonedBody(batch[i], error.what());
			}
		}
	}
}

void SemBinder::AppendPoisonedBody(const DeferredBody& body,
                                   const string& what)
{
	ESpecialFunction special = SF_NONE;
	if (body.decl->kind == DK_SPECIAL_MEMBER_DEFINITION &&
	    body.name.compare(0, 9, "operator ") != 0)
		special = body.name[0] == '~' ? SF_DESTRUCTOR : SF_CONSTRUCTOR;
	SemNodePtr item = BuildFunctionNode(body, special);
	item->instantiation_error = what;
	unit_.deferred.push_back(std::move(item));
}

// The this-adjusted printed type of a member function: pointer to cv
// class first, then the declared parameters.
TypePtr SemBinder::MethodAdjustedType(const ClassInfo& cls,
                                      const TypePtr& member)
{
	TypePtr class_type = MakeNamedType(TK_CLASS, cls.entity);
	class_type = MakeCvQualifiedType(class_type, member->is_const,
	                                 member->is_volatile);
	vector<TypePtr> parameters;
	parameters.push_back(MakePointerType(class_type, false, false));
	for (size_t i = 0; i < member->parameters.size(); i++)
		parameters.push_back(member->parameters[i]);
	TypePtr adjusted = MakeFunctionType(member->target, parameters,
	                                    member->variadic);
	if (member->ref_qual)
		adjusted = MakeRefQualifiedType(adjusted, member->ref_qual);
	return adjusted;
}

SemNodePtr SemBinder::BuildFunctionNode(const DeferredBody& body,
                                        ESpecialFunction special)
{
	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	bool is_method = !body.is_friend && !body.is_static;
	if (body.is_friend)
		item->name = CanonicalQualifiedName(body.declaring, body.name);
	else
		item->name = QualifiedScopePath(body.declaring) + body.name;
	item->entity_scope = body.declaring;
	item->entity_name = body.name;
	item->unwind_no = body.composed.noexcept_simple;
	item->inline_def = !body.out_of_class;
	item->is_method = is_method;
	item->special = special;
	if (is_method)
	{
		item->type = MethodAdjustedType(*body.cls, body.composed.type);
		SemNodePtr this_param = MakeSemNode(SN_PARAMETER);
		this_param->name = "this";
		this_param->type = item->type->parameters[0];
		this_param->entity_scope = body.fn_scope;
		this_param->entity_name = "this";
		item->children.push_back(std::move(this_param));
		if (!FindOwnBinding(*body.fn_scope, "this"))
		{
			ScopeBinding this_binding;
			this_binding.kind = SB_PARAMETER;
			this_binding.name = "this";
			this_binding.type = item->type->parameters[0];
			AddBinding(*body.fn_scope, this_binding);
		}
	}
	else
		item->type = body.composed.type;
	// PA18: an unnamed definition parameter borrows the declaration's
	// recorded name for its lowered slot.
	const vector<string>* declared_names = 0;
	if (const ScopeBinding* fn = FindOwnBinding(*body.declaring,
	                                            body.name))
		if (fn->kind == SB_FUNCTION)
		{
			size_t index = 0;
			for (size_t i = 0; i < fn->overloads.size(); i++)
				if (TypeEquals(fn->overloads[i], body.composed.type))
					index = i + 1;
			if (index < fn->fn_param_names.size())
				declared_names = &fn->fn_param_names[index];
		}
	for (size_t i = 0; i < body.composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = body.composed.parameters[i].name;
		if (parameter->name.empty() && declared_names &&
		    i < declared_names->size())
			parameter->name = (*declared_names)[i];
		parameter->type = body.composed.parameters[i].type;
		parameter->entity_scope = body.fn_scope;
		parameter->entity_name = parameter->name;
		AttachParameterDtor(*parameter);
		item->children.push_back(std::move(parameter));
	}
	return item;
}

void SemBinder::AttachParameterDtor(SemNode& parameter)
{
	TypePtr bare = RemoveTopCv(parameter.type);
	if (bare->kind != TK_CLASS || IsReferenceType(parameter.type))
		return;
	const ClassInfo* cls = unit_.classes.Find(bare->named);
	if (!cls || !unit_.classes.NeedsDestruction(*cls))
		return;
	SemNodePtr object = MakeSemNode(SN_ID_EXPRESSION);
	object->name = parameter.name;
	object->type = parameter.type;
	object->category = VC_LVALUE;
	object->entity_scope = parameter.entity_scope;
	object->entity_name = parameter.name;
	parameter.children.push_back(
		MakeDestructorCall(*cls, false,
		                   AddressOfNode(std::move(object))));
}

void SemBinder::AnalyzeDeferredBody(const DeferredBody& body)
{
	ESpecialFunction special = SF_NONE;
	if (body.decl->kind == DK_SPECIAL_MEMBER_DEFINITION &&
	    body.name.compare(0, 9, "operator ") != 0)
		special = body.name[0] == '~' ? SF_DESTRUCTOR : SF_CONSTRUCTOR;
	SemNodePtr item = BuildFunctionNode(body, special);
	SemNode* node = item.get();
	// 7.1.5: a constexpr member/friend body is engine-evaluable and
	// implicitly inline (weak, demand-emitted). Ordinary declarations
	// carry the keyword in the specifier-seq, special members in the
	// member-specifier list.
	for (size_t i = 0; i < body.decl->specifiers.size(); i++)
		if (body.decl->specifiers[i].kind == SPEC_KEYWORD &&
		    body.decl->specifiers[i].keyword == KW_CONSTEXPR)
		{
			node->is_constexpr_fn = true;
			node->inline_def = true;
		}
	for (size_t i = 0; i < body.decl->member_specifiers.size(); i++)
		if (body.decl->member_specifiers[i].keyword == KW_CONSTEXPR)
		{
			node->is_constexpr_fn = true;
			node->inline_def = true;
		}

	Scope* saved_scope = current_;
	EMemberAccess saved_access = current_access_;
	MethodContext saved_method = method_;
	TypePtr saved_return = current_return_;
	vector<SemNode*> saved_parents;
	saved_parents.swap(parents_);
	current_ = body.fn_scope;
	current_access_ = MA_PUBLIC;
	method_ = MethodContext();
	method_.fn_scope = body.fn_scope;
	method_.fn_name = body.name;
	method_.fn_owner = body.declaring;
	if (!body.is_friend)
	{
		method_.cls = body.cls;
		if (!body.is_static)
			method_.this_type = node->type->parameters[0];
	}
	else
		method_.lexical_cls = body.cls;
	current_return_ = special == SF_NONE ? body.composed.type->target
	                                     : MakeFundamentalType(FT_VOID);
	parents_.push_back(node);
	try
	{
		if (special == SF_CONSTRUCTOR)
			AnalyzeMemberInits(body, *node);
		// PA17: a polymorphic destructor re-stores this class's
		// vpointer before the body runs (12.4, 10.4p6 dispatch model).
		if (special == SF_DESTRUCTOR && body.cls->is_polymorphic)
			node->children.push_back(MakeVPointerStore(*body.cls));
		BindStatement(*body.decl->body);
		if (special == SF_DESTRUCTOR)
			AnalyzeDtorEpilogue(*body.cls, *node);
	}
	catch (...)
	{
		parents_.swap(saved_parents);
		current_ = saved_scope;
		current_access_ = saved_access;
		method_ = saved_method;
		current_return_ = saved_return;
		throw;
	}
	parents_.swap(saved_parents);
	current_ = saved_scope;
	current_access_ = saved_access;
	method_ = saved_method;
	current_return_ = saved_return;

	PublishBodyUnwindFact(body, special, *node);
	// PA18: an instantiated out-of-class member definition emits weak
	// and on demand, like an in-class one (14.7.1).
	if (body.out_of_class && special == SF_NONE && !instantiating_)
		AppendItem(std::move(item));
	else
	{
		if (body.out_of_class && !instantiating_)
			// A source-owned constructor prints unconditionally.
			node->inline_def = false;
		else if (body.out_of_class)
			// An instantiated out-of-class constructor/destructor
			// emits weak and on demand, like an in-class one (14.7.1).
			node->inline_def = true;
		unit_.deferred.push_back(std::move(item));
	}
}

void SemBinder::PublishBodyUnwindFact(const DeferredBody& body,
                                      ESpecialFunction special,
                                      SemNode& node)
{
	bool may_throw = false;
	for (size_t i = 0; i < node.children.size(); i++)
		if (NodeMayThrow(*node.children[i]))
			may_throw = true;
	if (may_throw)
		return;
	node.unwind_no = true;
	if (special == SF_DESTRUCTOR)
	{
		if (body.cls)
			body.cls->dtor_unwind_no = true;
		return;
	}
	if (special == SF_CONSTRUCTOR)
	{
		if (!body.cls)
			return;
		int index = ClassCtorIndex(*body.cls, body.composed.type);
		if (index >= 0)
			body.cls->ctors[index].unwind_no = true;
		return;
	}
	ScopeBinding* binding = FindOwnBinding(*body.declaring, body.name);
	if (!binding || binding->kind != SB_FUNCTION)
		return;
	size_t index = 0;
	for (size_t i = 0; i < binding->overloads.size(); i++)
		if (TypeEquals(binding->overloads[i], body.composed.type))
			index = i + 1;
	if (binding->fn_unwind_no.size() <= index)
		binding->fn_unwind_no.resize(index + 1, false);
	binding->fn_unwind_no[index] = true;
}

