#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// sem_class.cpp: whether a member definition spells `inline`.
bool DeclSpellsInline(const AstDecl& decl);

// PA15 member-function-body machinery, split from sem_class.cpp: the
// deferred in-class bodies (analyzed once the outermost class
// completes, 9.2p2), the shared function-node builder, and the
// per-body analysis context.

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
	// body analyzes immediately (and emits as a strong definition
	// unless spelled inline, 7.1.2p4).
	body.out_of_class = true;
	body.spelled_inline = DeclSpellsInline(decl);
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
	// The end-of-unit pass retries the bind once all classes complete.
	RetryBody retry;
	retry.body = body;
	retry.deferred_index = unit_.deferred.size() - 1;
	retry_bodies_.push_back(retry);
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
	item->from_instantiation = instantiating_;
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

// PA23 12.3.2: whether a just-bound conversion body performs no
// observable work - exactly a return of a default-constructed
// empty-class temporary whose construction chain is effect-free. The
// lowering reads the published fact to elide an empty-object
// copy-initialization through the conversion (the callee stays
// odr-used and prints on its ordinary demand).
static bool ConversionBodyPerformsNoWork(const SemNode& fn,
                                         const ClassRegistry& classes)
{
	const SemNode* compound = 0;
	for (size_t i = 0; i < fn.children.size(); i++)
	{
		if (fn.children[i]->kind == SN_PARAMETER)
			continue;
		if (compound)
			return false;
		compound = fn.children[i].get();
	}
	if (!compound || compound->kind != SN_COMPOUND_STATEMENT ||
	    compound->children.size() != 1 ||
	    compound->children[0]->kind != SN_RETURN_STATEMENT ||
	    compound->children[0]->children.size() != 1)
		return false;
	const SemNode& returned = *compound->children[0]->children[0];
	if (returned.kind != SN_CONSTRUCTOR_ACTION || !returned.type)
		return false;
	const NamedTypeInfo* named = RemoveTopCv(returned.type)->named;
	if (!named || !named->class_record || !named->class_record->is_empty)
		return false;
	// The construction takes the callee and at most the object
	// address - no value arguments - and its chain does nothing.
	return !returned.children.empty() &&
		returned.children[0]->kind == SN_CALL_EXPRESSION &&
		returned.children[0]->children.size() <= 2 &&
		!classes.DefaultConstructionHasSyntacticEffects(
			*named->class_record);
}

// The deferred body statement with its special-member framing: ctor
// member inits and dtor subobject destructions route into the try
// region of a function-try-block (15.2p11, 15.3p15), around the plain
// body otherwise.
void SemBinder::BindDeferredBodyStatement(const DeferredBody& body,
                                          ESpecialFunction special,
                                          SemNode& node)
{
	bool function_try = body.decl->body &&
		body.decl->body->kind == SK_TRY &&
		body.decl->body->function_try;
	bool ctor_function_try = special == SF_CONSTRUCTOR && function_try;
	bool dtor_function_try = special == SF_DESTRUCTOR && function_try;
	// A ctor-initializer parses on any function-try-block, but only
	// constructors may have one (12.6.2p1).
	if (special != SF_CONSTRUCTOR && body.decl->has_ctor_initializer)
		throw runtime_error(
			"ctor-initializer on a non-constructor function");
	if (special == SF_CONSTRUCTOR && !ctor_function_try)
		AnalyzeMemberInits(body, node);
	// PA17: a polymorphic destructor re-stores this class's vpointer
	// before the body runs (12.4, 10.4p6 dispatch model).
	if (special == SF_DESTRUCTOR && body.cls->is_polymorphic)
		node.children.push_back(MakeVPointerStore(*body.cls));
	if (ctor_function_try)
		BindTryStatement(*body.decl->body, &body);
	else if (dtor_function_try)
		BindTryStatement(*body.decl->body, 0, body.cls);
	else
		BindStatement(*body.decl->body);
	if (special == SF_DESTRUCTOR && !dtor_function_try)
		AnalyzeDtorEpilogue(*body.cls, node);
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
	// implicitly inline (weak, demand-emitted).
	if (DeclHasConstexpr(*body.decl))
	{
		node->is_constexpr_fn = true;
		node->inline_def = true;
	}

	Scope* saved_scope = current_;
	EMemberAccess saved_access = current_access_;
	MethodContext saved_method = method_;
	TypePtr saved_return = current_return_;
	TypePtr saved_pattern = auto_return_pattern_;
	int saved_hidden = range_hidden_counter_;
	range_hidden_counter_ = 0;
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
	auto_return_pattern_ = TypeContainsAutoPlaceholder(current_return_)
		? current_return_ : TypePtr();
	parents_.push_back(node);
	try
	{
		BindDeferredBodyStatement(body, special, *node);
	}
	catch (...)
	{
		parents_.swap(saved_parents);
		current_ = saved_scope;
		current_access_ = saved_access;
		method_ = saved_method;
		current_return_ = saved_return;
		auto_return_pattern_ = saved_pattern;
		range_hidden_counter_ = saved_hidden;
		throw;
	}
	TypePtr deduced_return = current_return_;
	parents_.swap(saved_parents);
	current_ = saved_scope;
	current_access_ = saved_access;
	method_ = saved_method;
	current_return_ = saved_return;
	auto_return_pattern_ = saved_pattern;
	range_hidden_counter_ = saved_hidden;

	DeferredBody published = body;
	if (special == SF_NONE &&
	    TypeContainsAutoPlaceholder(body.composed.type->target))
		published.composed.type = PublishDeducedMemberReturn(
			body, deduced_return, *node);
	PublishBodyUnwindFact(published, special, *node);
	// PA23: a conversion function (DK_SPECIAL_MEMBER_DEFINITION whose
	// name keeps the "operator " spelling) publishes whether its body
	// performs observable work; the lowering elides an empty-object
	// copy-initialization through a workless one.
	if (special == SF_NONE &&
	    body.decl->kind == DK_SPECIAL_MEMBER_DEFINITION)
		node->conversion_no_work =
			ConversionBodyPerformsNoWork(*node, unit_.classes);
	// PA18: an instantiated out-of-class member definition emits weak
	// and on demand, like an in-class one (14.7.1). PA23: a
	// source-owned spelled-inline one does too (7.1.2 with 3.2; the
	// explicit-specialization member shape emits nothing unused).
	if (body.out_of_class && special == SF_NONE && !instantiating_ &&
	    !body.spelled_inline)
		AppendItem(std::move(item));
	else
	{
		if (body.out_of_class && special == SF_NONE && !instantiating_)
			node->inline_def = true;
		else if (body.out_of_class && !instantiating_)
		{
			// A source-owned constructor prints unconditionally; a
			// spelled-inline one prints weak but still prints.
			node->inline_def = body.spelled_inline;
			node->inline_root = body.spelled_inline;
		}
		else if (body.out_of_class)
			// An instantiated out-of-class constructor/destructor
			// emits weak and on demand, like an in-class one (14.7.1).
			node->inline_def = true;
		unit_.deferred.push_back(std::move(item));
	}
}

// 7.1.6.4p7/p10: the member's placeholder return deduced from its
// body (void when no return statement ran); the definition node and
// the class-scope overload entry take the deduced signature, which is
// returned for the unwind-fact publication.
TypePtr SemBinder::PublishDeducedMemberReturn(const DeferredBody& body,
                                              TypePtr deduced_return,
                                              SemNode& node)
{
	if (TypeContainsAutoPlaceholder(deduced_return))
		deduced_return = MakeFundamentalType(FT_VOID);
	const TypePtr& spelled = body.composed.type;
	TypePtr fixed = MakeFunctionType(deduced_return,
	                                 spelled->parameters,
	                                 spelled->variadic);
	fixed = MakeFunctionCvQualifiedType(fixed, spelled->is_const,
	                                    spelled->is_volatile);
	if (spelled->ref_qual)
		fixed = MakeRefQualifiedType(fixed, spelled->ref_qual);
	node.type = body.is_friend || body.is_static
		? fixed : MethodAdjustedType(*body.cls, fixed);
	if (ScopeBinding* fn = FindOwnBinding(*body.declaring, body.name))
	{
		if (fn->type && TypeEquals(fn->type, spelled))
			fn->type = fixed;
		else
			for (size_t i = 0; i < fn->overloads.size(); i++)
				if (TypeEquals(fn->overloads[i], spelled))
					fn->overloads[i] = fixed;
	}
	return fixed;
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

