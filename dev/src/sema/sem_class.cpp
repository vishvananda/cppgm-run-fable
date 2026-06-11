#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 class-side machinery of the semantic binder: ClassInfo
// recording and layout, base clauses, special members, friends,
// bit-fields, and the deferred analysis of in-class member-function
// bodies (9.2p2: the class is complete within them).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

}  // namespace

ClassInfo* SemBinder::OpenClass() const
{
	return open_classes_.empty() ? 0 : open_classes_.back();
}

void SemBinder::OnClassOpened(const AstDecl& decl, NamedTypeInfo* info,
                              Scope* scope)
{
	(void)decl;
	ClassInfo& cls = unit_.classes.Create(info);
	cls.members = scope;
	cls.is_union = info->is_union;
	BeginClassLayout(cls);
	open_classes_.push_back(&cls);
}

void SemBinder::BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
                               Scope* scope)
{
	(void)info;
	ClassInfo* cls = OpenClass();
	if (decl.bases.size() != 1)
		throw OutsideBoundary("multiple inheritance");
	const AstBaseSpecifier& base = decl.bases[0];
	if (base.is_virtual)
		throw OutsideBoundary("virtual inheritance");
	if (base.pack)
		throw OutsideBoundary("base pack expansion");
	TypePtr base_type = ResolveTypeName(base.name);
	if (base_type->kind != TK_CLASS || !base_type->named->complete)
		throw runtime_error("base class is not a complete class");
	ClassInfo* base_cls = unit_.classes.Find(base_type->named);
	if (!base_cls)
		throw runtime_error("base class record missing");
	cls->base = base_cls;
	model_.MutableInfo(info)->base_entity = base_cls->entity;
	// 11.2p2: the default base access is private for `class` keys and
	// public otherwise.
	if (base.has_access)
		cls->base_access = base.access == KW_PRIVATE ? MA_PRIVATE
			: base.access == KW_PROTECTED ? MA_PROTECTED : MA_PUBLIC;
	else
		cls->base_access =
			decl.class_key == KW_CLASS ? MA_PRIVATE : MA_PUBLIC;
	// 8.5.1p1: a class with bases is not an aggregate.
	cls->is_aggregate = false;
	// Base members become reachable through unqualified and qualified
	// member lookup (10.2 subset: single inheritance, no hiding merge).
	scope->class_base = base_cls->members;
	BeginClassLayout(*cls);
}

void SemBinder::RecordMemberField(ScopeBinding& binding,
                                  const AstInitializer* init,
                                  const DeclSpecifierInfo& specs)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		return;  // a nested non-class scope is not a member region
	if (specs.is_static)
	{
		if (specs.is_mutable)
			throw runtime_error("static member declared mutable");
		return;  // static data members are not fields
	}
	if (in_bit_field_)
		return;  // the bit-field path lays its own rows
	ClassField field;
	field.name = binding.name;
	field.type = binding.type;
	field.is_mutable = specs.is_mutable;
	field.access = binding.access;
	field.default_init = init;
	if (init)
		// C++11 8.5.1p1: a default member initializer disqualifies
		// aggregate initialization.
		cls->is_aggregate = false;
	if (binding.access != MA_PUBLIC)
		cls->is_aggregate = false;
	LayoutField(*cls, field);
}

void SemBinder::BindBitFieldDeclaration(const AstDecl& decl)
{
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	if (specs.is_typedef)
		throw runtime_error("typedef bit-field");
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw runtime_error("bit-field outside a class member region");
	for (size_t i = 0; i < decl.bit_fields.size(); i++)
	{
		const AstDecl::BitField& bit_field = decl.bit_fields[i];
		TypePtr type = specs.type;
		string name;
		if (bit_field.declarator)
		{
			DeclaratorInfo composed = builder_.ComposeDeclarator(
				bit_field.declarator.get(), specs.type);
			if (!composed.id || !composed.id->IsPlainIdentifier())
				throw OutsideBoundary("bit-field declarator");
			type = composed.type;
			name = composed.id->parts[0].identifier;
		}
		// 9.6p3: ordinary integral and enumeration bit-fields.
		if (!IsIntegralType(type) && type->kind != TK_ENUM)
			throw runtime_error("bit-field of a non-integral type");
		ConstValue width_value =
			EvaluateConstExpr(*bit_field.width, *this);
		if (IsSignedIntegralFundamental(width_value.type) &&
		    (long long)width_value.bits < 0)
			throw runtime_error("negative bit-field width");
		if (!name.empty() && width_value.bits == 0)
			throw runtime_error("zero-width named bit-field");
		if (width_value.bits > TypeSize(type) * 8)
			throw OutsideBoundary("oversized bit-field width");
		ClassField field;
		field.name = name;
		field.type = type;
		field.is_mutable = specs.is_mutable;
		field.access = current_access_;
		field.bit_width = width_value.bits;
		LayoutBitField(*cls, field);
		if (!name.empty())
		{
			in_bit_field_ = true;
			try
			{
				BindVariable(name, type, 0, specs);
			}
			catch (...)
			{
				in_bit_field_ = false;
				throw;
			}
			in_bit_field_ = false;
		}
	}
}

void SemBinder::CompleteClass(const AstDecl& decl, NamedTypeInfo* info,
                              Scope* scope, const std::vector<TypePtr>& fields)
{
	(void)decl;
	(void)scope;
	(void)fields;
	ClassInfo* cls = OpenClass();
	FinishClassLayout(*cls, *info);
	info->complete = true;
	open_classes_.pop_back();
	if (open_classes_.empty())
		FlushDeferredBodies();
}

// --- special members --------------------------------------------------

void SemBinder::BindSpecialMember(const AstDecl& decl)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw OutsideBoundary("out-of-class special member definition");
	const AstName* id = decl.declarator->IdName();
	if (!id || id->parts.size() != 1)
		throw OutsideBoundary("special member name form");
	const AstNamePart& part = id->parts[0];
	if (part.kind != NP_IDENTIFIER)
		throw OutsideBoundary("conversion function");
	if (part.identifier != cls->members->name)
		throw runtime_error("special member does not name its class");
	bool is_dtor = part.tilde;
	bool is_explicit = false;
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
	{
		ETokenType keyword = decl.member_specifiers[i].keyword;
		if (keyword == KW_EXPLICIT)
			is_explicit = true;
		else if (keyword == KW_VIRTUAL)
			throw OutsideBoundary("virtual special member");
		else if (keyword == KW_STATIC || keyword == KW_FRIEND)
			throw runtime_error("invalid specifier on a special member");
	}
	// Composing the declarator over void yields exactly the
	// constructor/destructor function type over the declared parameters.
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		decl.declarator.get(), MakeFundamentalType(FT_VOID));
	if (composed.type->kind != TK_FUNCTION)
		throw runtime_error("special member requires a parameter clause");
	bool defaulted = decl.special_init &&
		decl.special_init->kind == INIT_DEFAULT;
	bool deleted = decl.special_init &&
		decl.special_init->kind == INIT_DELETE;
	bool defined = decl.kind == DK_SPECIAL_MEMBER_DEFINITION;
	if (is_dtor)
	{
		if (!composed.type->parameters.empty() || composed.type->variadic)
			throw runtime_error("destructor with parameters");
		if (decl.has_ctor_initializer)
			throw runtime_error("destructor with a ctor-initializer");
		// A defaulted destructor behaves like the implicit one.
		cls->has_user_dtor = !defaulted && !deleted;
		cls->dtor_access = current_access_;
		cls->dtor_deleted = deleted;
		cls->dtor_definition = defined ? &decl : 0;
		cls->dtor_unwind_no = composed.noexcept_simple;
		if (defined)
		{
			DeferredBody body;
			body.decl = &decl;
			body.composed = composed;
			body.name = "~" + cls->members->name;
			body.fn_scope = MakeSpecialMemberScope(body.name, composed,
			                                       *cls);
			body.declaring = cls->members;
			body.cls = cls;
			deferred_bodies_.push_back(body);
		}
		return;
	}
	ClassCtor ctor;
	ctor.type = composed.type;
	ctor.access = current_access_;
	ctor.is_explicit = is_explicit;
	ctor.deleted = deleted;
	ctor.defaulted = defaulted;
	ctor.unwind_no = composed.noexcept_simple;
	ctor.definition = defined ? &decl : 0;
	for (size_t i = 0; i < composed.parameters.size(); i++)
		ctor.defaults.push_back(composed.parameters[i].default_arg);
	cls->ctors.push_back(ctor);
	// 8.5.1p1/12.1p5: user-provided or deleted constructors disqualify
	// aggregates; `= default` on the default constructor keeps the
	// class an aggregate (C++11 semantics pinned by the fixtures).
	if (!defaulted)
		cls->is_aggregate = false;
	if (!defaulted && !deleted)
		cls->has_user_ctor = true;
	if (defined)
	{
		DeferredBody body;
		body.decl = &decl;
		body.composed = composed;
		body.name = cls->members->name;
		body.fn_scope = MakeSpecialMemberScope(body.name, composed, *cls);
		body.declaring = cls->members;
		body.cls = cls;
		deferred_bodies_.push_back(body);
	}
}

// The function scope of a constructor/destructor body, with its
// declared parameters bound.
Scope* SemBinder::MakeSpecialMemberScope(const string& name,
                                         const DeclaratorInfo& composed,
                                         ClassInfo& cls)
{
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, name,
	                                     cls.members);
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		const ParameterInfo& parameter = composed.parameters[i];
		if (parameter.name.empty())
			continue;
		ScopeBinding param_binding;
		param_binding.kind = SB_PARAMETER;
		param_binding.name = parameter.name;
		param_binding.type = parameter.type;
		AddBinding(*fn_scope, param_binding);
	}
	return fn_scope;
}

// --- friends ------------------------------------------------------------

void SemBinder::BindFriendDeclaration(const AstDecl& decl)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw runtime_error("friend declaration outside a class");
	// `friend class X;` / `friend struct Y;`: the elaborated type
	// specifier names (or forward-declares) the befriended class.
	bool has_nested = false;
	for (size_t i = 0; i < decl.specifiers.size(); i++)
		if (decl.specifiers[i].kind == SPEC_NESTED_DECL)
			has_nested = true;
	if (decl.kind == DK_SIMPLE && decl.declarators.empty())
	{
		if (!has_nested)
			return;  // friend of a non-class type declares nothing
		Scope* saved = current_;
		current_ = EnclosingNamespace();
		DeclSpecifierInfo specs;
		try
		{
			specs = builder_.ProcessSpecifiers(decl.specifiers, true);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		if (specs.type->kind == TK_CLASS)
		{
			Scope* members = model_.MemberScope(specs.type->named);
			if (members)
				cls->friend_classes.push_back(members);
		}
		return;
	}
	BindFriendFunction(decl, *cls);
}

Scope* SemBinder::EnclosingNamespace()
{
	Scope* scope = current_;
	while (scope && scope->kind != SCOPE_NAMESPACE)
		scope = scope->parent;
	return scope;
}

// A friend function declaration or hidden-friend definition: the name
// declares into the innermost enclosing namespace (11.3p6) but stays
// invisible to ordinary lookup until a matching namespace-scope
// declaration appears (7.3.1.2p3); ADL still finds it.
void SemBinder::BindFriendFunction(const AstDecl& decl, ClassInfo& cls)
{
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(decl.specifiers, true);
	const AstDeclarator* declarator = 0;
	const AstInitDeclarator* init_declarator = 0;
	if (decl.kind == DK_FUNCTION)
		declarator = decl.declarator.get();
	else
	{
		if (decl.declarators.size() != 1)
			throw OutsideBoundary("friend declarator list");
		init_declarator = &decl.declarators[0];
		declarator = init_declarator->declarator.get();
	}
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(declarator, specs.type);
	if (!composed.id || composed.type->kind != TK_FUNCTION)
		throw runtime_error("friend declarator is not a function");
	const string name = DeclaredFunctionName(composed.id->parts.back());
	Scope* target;
	bool qualified = composed.id->parts.size() > 1 ||
		composed.id->global_scope;
	bool adl_only = !qualified;
	if (qualified)
	{
		// A qualified friend names an already-declared function; it
		// befriends without redeclaring (and never hides).
		Scope* saved = current_;
		current_ = EnclosingNamespace();
		try
		{
			target = ResolvePrefixScope(*composed.id);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		if (target->kind != SCOPE_NAMESPACE ||
		    !FindOwnBinding(*target, name))
			throw runtime_error("qualified friend is not declared");
	}
	else
		target = EnclosingNamespace();
	cls.friend_functions.push_back(std::make_pair(target, name));
	if (qualified && decl.kind != DK_FUNCTION)
		return;
	Scope* saved = current_;
	current_ = target;
	EMemberAccess saved_access = current_access_;
	current_access_ = MA_PUBLIC;
	try
	{
		ScopeBinding& binding = BindFunctionName(name, composed.type,
		                                         false);
		bool deleted = init_declarator && init_declarator->init &&
			init_declarator->init->kind == INIT_DELETE;
		RecordFunctionFacts(binding, composed, deleted, &specs,
		                    decl.kind == DK_FUNCTION, adl_only);
	}
	catch (...)
	{
		current_ = saved;
		current_access_ = saved_access;
		throw;
	}
	current_ = saved;
	current_access_ = saved_access;
	if (decl.kind == DK_FUNCTION)
	{
		// A hidden-friend definition: the body is analyzed against the
		// class's lexical scope once the outermost class completes.
		Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, name,
		                                     cls.members);
		for (size_t i = 0; i < composed.parameters.size(); i++)
		{
			const ParameterInfo& parameter = composed.parameters[i];
			if (parameter.name.empty())
				continue;
			ScopeBinding param_binding;
			param_binding.kind = SB_PARAMETER;
			param_binding.name = parameter.name;
			param_binding.type = parameter.type;
			AddBinding(*fn_scope, param_binding);
		}
		DeferredBody body;
		body.decl = &decl;
		body.composed = composed;
		body.name = name;
		body.fn_scope = fn_scope;
		body.declaring = target;
		body.cls = &cls;
		body.is_friend = true;
		deferred_bodies_.push_back(body);
	}
}

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
	AnalyzeDeferredBody(body);
}

void SemBinder::FlushDeferredBodies()
{
	while (!deferred_bodies_.empty())
	{
		vector<DeferredBody> batch;
		batch.swap(deferred_bodies_);
		for (size_t i = 0; i < batch.size(); i++)
			AnalyzeDeferredBody(batch[i]);
	}
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
	return MakeFunctionType(member->target, parameters, member->variadic);
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
	for (size_t i = 0; i < body.composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = body.composed.parameters[i].name;
		parameter->type = body.composed.parameters[i].type;
		parameter->entity_scope = body.fn_scope;
		parameter->entity_name = parameter->name;
		item->children.push_back(std::move(parameter));
	}
	return item;
}

void SemBinder::AnalyzeDeferredBody(const DeferredBody& body)
{
	ESpecialFunction special = SF_NONE;
	if (body.decl->kind == DK_SPECIAL_MEMBER_DEFINITION)
		special = body.name[0] == '~' ? SF_DESTRUCTOR : SF_CONSTRUCTOR;
	SemNodePtr item = BuildFunctionNode(body, special);
	SemNode* node = item.get();

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

	if (body.out_of_class)
		AppendItem(std::move(item));
	else
		unit_.deferred.push_back(std::move(item));
}

// --- object-model host services -----------------------------------------

ClassRegistry& SemBinder::Classes()
{
	return unit_.classes;
}

const ClassInfo* SemBinder::CurrentClass()
{
	return method_.cls;
}

TypePtr SemBinder::CurrentThisType()
{
	return method_.this_type;
}

void SemBinder::CheckMemberAccess(const Scope* owner, EMemberAccess access,
                                  const string& what)
{
	if (access == MA_PUBLIC)
		return;
	const NamedTypeInfo* owner_entity = model_.ScopeEntity(owner);
	const ClassInfo* owner_cls =
		owner_entity ? unit_.classes.Find(owner_entity) : 0;
	if (!owner_cls)
		return;
	vector<const ClassInfo*> contexts;
	if (method_.cls)
		contexts.push_back(method_.cls);
	if (method_.lexical_cls)
		contexts.push_back(method_.lexical_cls);
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS)
			if (const NamedTypeInfo* entity = model_.ScopeEntity(scope))
				if (const ClassInfo* cls = unit_.classes.Find(entity))
					contexts.push_back(cls);
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i]->members == owner)
			return;
		if (access == MA_PROTECTED)
			for (const ClassInfo* link = contexts[i]; link;
			     link = link->base)
				if (link->members == owner)
					return;
		// A friend class's members access everything (11.3).
		for (size_t j = 0; j < owner_cls->friend_classes.size(); j++)
			if (owner_cls->friend_classes[j] == contexts[i]->members)
				return;
	}
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < owner_cls->friend_functions.size(); i++)
			if (owner_cls->friend_functions[i].first == method_.fn_owner &&
			    owner_cls->friend_functions[i].second == method_.fn_name)
				return;
	throw runtime_error(what + " is inaccessible in this context");
}

// --- node builders --------------------------------------------------------

SemNodePtr SemBinder::ThisObjectExpr()
{
	SemNodePtr id = MakeSemNode(SN_ID_EXPRESSION);
	id->name = "this";
	id->type = method_.this_type;
	id->category = VC_PRVALUE;
	id->entity_scope = method_.fn_scope;
	id->entity_name = "this";
	SemNodePtr deref = MakeSemNode(SN_UNARY_EXPRESSION);
	deref->type = method_.this_type->target;
	deref->category = VC_LVALUE;
	deref->has_op = true;
	deref->op = OP_STAR;
	deref->op_spelling = "*";
	deref->children.push_back(std::move(id));
	return deref;
}

SemNodePtr SemBinder::ThisFieldExpr(const ClassField& field)
{
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
	member->children.push_back(ThisObjectExpr());
	return member;
}

SemNodePtr SemBinder::ThisBaseAddress(const ClassInfo& cls)
{
	SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
	member->type = MakeNamedType(TK_CLASS, cls.base->entity);
	member->category = VC_LVALUE;
	member->base_hops = 1;
	member->children.push_back(ThisObjectExpr());
	return AddressOfNode(std::move(member));
}

SemNodePtr SemBinder::AddressOfNode(SemNodePtr operand)
{
	SemNodePtr address = MakeSemNode(SN_UNARY_EXPRESSION);
	address->type = MakePointerType(RemoveTopCv(operand->type), false,
	                                false);
	address->category = VC_PRVALUE;
	address->has_op = true;
	address->op = OP_AMP;
	address->op_spelling = "&";
	address->children.push_back(std::move(operand));
	return address;
}

SemNodePtr SemBinder::SubscriptNode(SemNodePtr array,
                                    unsigned long long index)
{
	SemNodePtr literal = MakeSemNode(SN_LITERAL);
	literal->token = std::to_string(index);
	literal->type = MakeFundamentalType(FT_INT);
	literal->category = VC_PRVALUE;
	literal->has_value = true;
	literal->value = ConstValue(FT_INT, index);
	SemNodePtr node = MakeSemNode(SN_SUBSCRIPT_EXPRESSION);
	node->type = array->type->target;
	node->category = VC_LVALUE;
	node->children.push_back(std::move(array));
	node->children.push_back(std::move(literal));
	return node;
}

// A scalar/reference member initialization: an assignment-shaped store
// of the converted value into the member lvalue (the lowering stores
// addresses for reference members and masks bit-fields).
SemNodePtr SemBinder::MemberAssignAction(const ClassField& field,
                                         SemNodePtr lhs, SemValue value)
{
	if (IsReferenceType(field.type))
	{
		if (value.category != VC_LVALUE)
			throw runtime_error("reference member binds a non-lvalue");
		TypePtr referee = field.type->target;
		if (!TypeEquals(RemoveTopCv(referee), RemoveTopCv(value.type)) &&
		    !DerivedToBaseClass(value.type, referee))
			throw runtime_error("reference member binding mismatch");
	}
	else
		analyzer_.CopyInitialize(value, RemoveTopCv(field.type),
		                         "member initializer");
	SemNodePtr assign = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
	assign->type = lhs->type;
	assign->category = VC_LVALUE;
	assign->has_op = true;
	assign->op = OP_ASS;
	assign->op_spelling = "=";
	// A reference member initializer stores the referent's address
	// into the field itself.
	assign->member_ref = IsReferenceType(field.type);
	if (field.is_bit_field)
	{
		unsigned long long unit = field.offset;
		assign->is_bit_field = true;
		assign->bit_offset = field.bit_offset;
		assign->bit_width = field.bit_width;
		if (!bf_units_written_.count(unit))
		{
			assign->bf_plain_store = true;
			bf_units_written_[unit] = true;
		}
	}
	assign->children.push_back(std::move(lhs));
	assign->children.push_back(std::move(value.node));
	SemNodePtr statement = MakeSemNode(SN_EXPRESSION_STATEMENT);
	statement->children.push_back(std::move(assign));
	return statement;
}

// 4.10p3 derived-to-base over the single-inheritance chain.
bool SemBinder::DerivedToBaseClass(const TypePtr& from, const TypePtr& to)
{
	if (from->kind != TK_CLASS || to->kind != TK_CLASS)
		return false;
	const ClassInfo* cls = unit_.classes.Find(from->named);
	for (; cls; cls = cls->base)
		if (cls->entity == to->named)
			return true;
	return false;
}

// --- constructor selection ------------------------------------------------

int SemBinder::ResolveClassConstructor(const ClassInfo& cls,
                                       vector<SemValue>& args,
                                       bool copy_init, const char* what)
{
	if (cls.ctors.empty())
	{
		if (!args.empty())
			throw runtime_error(string("no matching constructor for ") +
			                    what);
		return -1;
	}
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		candidates.push_back(cls.ctors[i].type);
		size_t required = cls.ctors[i].type->parameters.size();
		const vector<const AstExpr*>& defaults = cls.ctors[i].defaults;
		while (required > 0 && required <= defaults.size() &&
		       defaults[required - 1])
			required--;
		min_arity.push_back(required);
	}
	vector<ConversionSource> sources;
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	vector<ImplicitConversion> conversions;
	size_t winner = SelectBestOverload(candidates, sources, conversions,
	                                   &min_arity);
	const ClassCtor& ctor = cls.ctors[winner];
	if (ctor.deleted)
		throw runtime_error(string("use of deleted constructor for ") +
		                    what);
	if (copy_init && ctor.is_explicit)
		throw runtime_error(string("explicit constructor selected by "
		                           "copy-initialization for ") + what);
	CheckMemberAccess(cls.members, ctor.access, "constructor");
	const TypePtr& fn = candidates[winner];
	for (size_t i = 0; i < args.size(); i++)
		if (i < fn->parameters.size())
			analyzer_.ApplyConversion(args[i], conversions[i],
			                          fn->parameters[i]);
	for (size_t i = args.size(); i < fn->parameters.size(); i++)
	{
		SemValue filled = analyzer_.Analyze(*ctor.defaults[i]);
		analyzer_.CopyInitialize(filled, fn->parameters[i],
		                         "default argument");
		args.push_back(std::move(filled));
	}
	if (ctor.defaulted && fn->parameters.empty())
		return -1;
	return (int)winner;
}

SemNodePtr SemBinder::MakeConstructorCall(const ClassInfo& cls,
                                          int ctor_index, bool base_entry,
                                          SemNodePtr address,
                                          vector<SemNodePtr> args)
{
	TypePtr ctor_type;
	bool callee_unwind_no = false;
	if (ctor_index < 0)
	{
		EnsureImplicitDefaultCtor(cls);
		ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
		                             vector<TypePtr>(), false);
		callee_unwind_no = cls.implicit_ctor_unwind_no;
	}
	else
	{
		ctor_type = cls.ctors[ctor_index].type;
		callee_unwind_no = cls.ctors[ctor_index].unwind_no;
	}
	TypePtr adjusted = MethodAdjustedType(cls, ctor_type);
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::" + base_name;
	SemNodePtr action = MakeSemNode(SN_CONSTRUCTOR_ACTION);
	action->name = qualified;
	action->special = base_entry ? SF_CONSTRUCTOR_BASE : SF_CONSTRUCTOR;
	action->trivial_init = ctor_index < 0 &&
		!unit_.classes.NeedsConstruction(cls);
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = MakeFundamentalType(FT_VOID);
	call->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = qualified;
	callee->type = adjusted;
	callee->entity_scope = cls.members;
	callee->entity_name = base_name;
	callee->is_method = true;
	callee->special = action->special;
	callee->unwind_no = callee_unwind_no;
	call->children.push_back(std::move(callee));
	call->children.push_back(std::move(address));
	for (size_t i = 0; i < args.size(); i++)
		call->children.push_back(std::move(args[i]));
	action->children.push_back(std::move(call));
	return action;
}

SemNodePtr SemBinder::MakeDestructorCall(const ClassInfo& cls,
                                         bool base_entry,
                                         SemNodePtr address)
{
	if (!cls.has_user_dtor)
		EnsureImplicitDtor(cls);
	if (cls.dtor_deleted)
		throw runtime_error("use of deleted destructor");
	CheckMemberAccess(cls.members, cls.dtor_access, "destructor");
	TypePtr dtor_type = MethodAdjustedType(
		cls, MakeFunctionType(MakeFundamentalType(FT_VOID),
		                      vector<TypePtr>(), false));
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::~" + base_name;
	SemNodePtr action = MakeSemNode(SN_DESTRUCTOR_ACTION);
	action->name = qualified;
	action->special = base_entry ? SF_DESTRUCTOR_BASE : SF_DESTRUCTOR;
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = MakeFundamentalType(FT_VOID);
	call->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = qualified;
	callee->type = dtor_type;
	callee->entity_scope = cls.members;
	callee->entity_name = "~" + base_name;
	callee->is_method = true;
	callee->special = action->special;
	callee->unwind_no = cls.has_user_dtor ? cls.dtor_unwind_no
	                                      : cls.implicit_dtor_unwind_no;
	call->children.push_back(std::move(callee));
	call->children.push_back(std::move(address));
	action->children.push_back(std::move(call));
	return action;
}

// --- constructor member initialization -------------------------------------

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
	if (member_cls && braced && member_cls->is_aggregate)
	{
		SemNodePtr proto = ThisFieldExpr(field);
		AppendAggregateInit(*member_cls, *proto, *braced, out);
		return;
	}
	if (member_cls)
	{
		// Class member: direct- or list-initialization by constructor.
		vector<SemValue> values;
		if (braced)
			for (size_t i = 0; i < braced->arguments.size(); i++)
				values.push_back(analyzer_.Analyze(*braced->arguments[i]));
		else
			for (size_t i = 0; i < args.size(); i++)
				values.push_back(analyzer_.Analyze(*args[i]));
		int index = ResolveClassConstructor(*member_cls, values, false,
		                                    field.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < values.size(); i++)
			arg_nodes.push_back(std::move(values[i].node));
		out.push_back(MakeConstructorCall(
			*member_cls, index, false,
			AddressOfNode(ThisFieldExpr(field)), std::move(arg_nodes)));
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
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
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			out.push_back(MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				vector<SemNodePtr>()));
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
		if (bare->target->kind == TK_ARRAY)
			throw OutsideBoundary("multidimensional class member array");
		for (unsigned long long i = 0; i < bare->bound; i++)
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			out.push_back(MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(SubscriptNode(ThisFieldExpr(field), i)),
				vector<SemNodePtr>()));
		}
		return;
	}
	if (IsReferenceType(field.type))
		throw runtime_error("reference member is not initialized");
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
		else if (cls.base->has_user_ctor ||
		         unit_.classes.NeedsConstruction(*cls.base))
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*cls.base, no_args, false,
			                                    "base initializer");
			actions.push_back(MakeConstructorCall(*cls.base, index, true,
			                                      ThisBaseAddress(cls),
			                                      vector<SemNodePtr>()));
		}
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
		else
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
			    !unit_.classes.NeedsDestruction(*member_cls))
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
		if (!member_cls || !unit_.classes.NeedsDestruction(*member_cls))
			continue;
		actions.push_back(MakeDestructorCall(
			*member_cls, false, AddressOfNode(ThisFieldExpr(field))));
	}
	if (cls.base && unit_.classes.NeedsDestruction(*cls.base))
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

void SemBinder::EnsureImplicitDefaultCtor(const ClassInfo& cls_in)
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
		if (cls.base && (cls.base->has_user_ctor ||
		                 unit_.classes.NeedsConstruction(*cls.base)))
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*cls.base, no_args, false,
			                                    "base subobject");
			actions.push_back(MakeConstructorCall(*cls.base, index, true,
			                                      ThisBaseAddress(cls),
			                                      vector<SemNodePtr>()));
		}
		for (size_t i = 0; i < cls.fields.size(); i++)
			if (!cls.fields[i].name.empty() || !cls.fields[i].is_bit_field)
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

void SemBinder::EnsureImplicitDtor(const ClassInfo& cls_in)
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
	// 8.5.1: members in declaration order; trailing members
	// value-initialize. Private members or user constructors already
	// disqualified the class from this path.
	size_t next = 0;
	bf_units_written_.clear();
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		SemNodePtr target = CloneSemNode(target_proto);
		SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
		member->name = field.name;
		member->type = field.type;
		member->category = VC_LVALUE;
		member->member_offset = field.offset;
		member->is_bit_field = field.is_bit_field;
		member->bit_offset = field.bit_offset;
		member->bit_width = field.bit_width;
		member->children.push_back(std::move(target));
		const AstExpr* element = next < braced.arguments.size()
			? braced.arguments[next].get() : 0;
		TypePtr bare = RemoveTopCv(field.type);
		const ClassInfo* member_cls = bare->kind == TK_CLASS
			? unit_.classes.Find(bare->named) : 0;
		if (member_cls)
		{
			if (element && element->kind == EK_BRACED)
			{
				next++;
				AppendAggregateInit(*member_cls, *member, *element, out);
			}
			else if (member_cls->is_aggregate)
			{
				// Brace elision: the nested aggregate consumes the
				// following initializers in order.
				AstExpr empty(EK_BRACED);
				if (element)
					throw OutsideBoundary("brace-elided aggregate member");
				AppendAggregateInit(*member_cls, *member, empty, out);
			}
			else
			{
				vector<SemValue> values;
				if (element)
				{
					next++;
					values.push_back(analyzer_.Analyze(*element));
				}
				int index = ResolveClassConstructor(
					*member_cls, values, true, field.name.c_str());
				vector<SemNodePtr> arg_nodes;
				for (size_t j = 0; j < values.size(); j++)
					arg_nodes.push_back(std::move(values[j].node));
				out.push_back(MakeConstructorCall(
					*member_cls, index, false,
					AddressOfNode(std::move(member)),
					std::move(arg_nodes)));
			}
			continue;
		}
		if (bare->kind == TK_ARRAY)
		{
			TypePtr element_type = RemoveTopCv(bare->target);
			if (element_type->kind == TK_CLASS)
				throw OutsideBoundary("aggregate class array member");
			if (element && element->kind == EK_BRACED)
			{
				next++;
				for (unsigned long long j = 0; j < bare->bound; j++)
				{
					ClassField element_field;
					element_field.name = field.name;
					element_field.type = element_type;
					SemNodePtr target_element =
						SubscriptNode(CloneSemNode(*member), j);
					if (j < element->arguments.size())
					{
						SemValue value = analyzer_.Analyze(
							*element->arguments[j]);
						CheckListInitNarrowing(value, element_type);
						out.push_back(MemberAssignAction(
							element_field, std::move(target_element),
							std::move(value)));
					}
					else
						out.push_back(MemberAssignAction(
							element_field, std::move(target_element),
							ZeroValue(element_type)));
				}
			}
			else if (!element)
			{
				for (unsigned long long j = 0; j < bare->bound; j++)
				{
					ClassField element_field;
					element_field.name = field.name;
					element_field.type = element_type;
					out.push_back(MemberAssignAction(
						element_field,
						SubscriptNode(CloneSemNode(*member), j),
						ZeroValue(element_type)));
				}
			}
			else
				throw OutsideBoundary("brace-elided array member");
			continue;
		}
		ClassField row = field;
		if (element)
		{
			next++;
			SemValue value = analyzer_.Analyze(*element);
			if (!IsReferenceType(field.type))
				CheckListInitNarrowing(value, bare);
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 std::move(value)));
		}
		else
		{
			if (IsReferenceType(field.type))
				throw runtime_error("reference member is not initialized");
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 ZeroValue(bare)));
		}
	}
	if (next < braced.arguments.size())
		throw runtime_error("too many initializers for aggregate");
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
	TypePtr type = binding.type;
	if (type->kind == TK_ARRAY)
	{
		if (init)
			throw OutsideBoundary("class array initializer");
		if (!cls.has_user_ctor && !unit_.classes.NeedsConstruction(cls))
			return;
		for (unsigned long long i = 0; i < type->bound; i++)
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(cls, no_args, false,
			                                    binding.name.c_str());
			item.children.push_back(MakeConstructorCall(
				cls, index, false,
				AddressOfNode(SubscriptNode(VariableObjectExpr(binding),
				                            i)),
				vector<SemNodePtr>()));
		}
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
			EnsureDefaultConstructor(RemoveTopCv(type), ctor_type);
		}
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    binding.name.c_str());
		item.children.push_back(MakeConstructorCall(
			cls, index, false,
			AddressOfNode(VariableObjectExpr(binding)),
			vector<SemNodePtr>()));
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
	    RemoveTopCv(values[0].type)->kind == TK_CLASS)
		throw OutsideBoundary("class copy-initialization");
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
