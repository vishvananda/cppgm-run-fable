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
	ClassInfo& cls = unit_.classes.Create(info);
	cls.members = scope;
	cls.is_union = info->is_union;
	model_.MutableInfo(info)->class_record = &cls;
	// PA17: pre-scan the body so the vpointer reserves offset 0 before
	// any field lays out (10.3p1: introducing a virtual member spells
	// the keyword; overriding without it needs a polymorphic base).
	cls.declares_virtual = ClassBodyDeclaresVirtual(decl);
	if (cls.declares_virtual)
	{
		if (cls.is_union)
			throw runtime_error("virtual member in a union");
		cls.is_aggregate = false;
	}
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
	// PA17: the derived class inherits the base's vtable slots (an
	// overrider replaces in place) and the virtual-destructor facts.
	// Introducing the first vpointer over a non-empty non-polymorphic
	// base would need base-subobject pointer adjustment (out of scope).
	cls->vslots = base_cls->vslots;
	cls->dtor_slot = base_cls->dtor_slot;
	if (cls->declares_virtual && !base_cls->is_polymorphic &&
	    !base_cls->is_empty)
		throw OutsideBoundary("vpointer introduction over a non-empty "
		                      "non-polymorphic base");
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
	(void)scope;
	(void)fields;
	ClassInfo* cls = OpenClass();
	FinishClassLayout(*cls, *info, RequestedAlignment(decl));
	info->complete = true;
	FinishClassVirtualFacts(*cls);
	DeclareImplicitSpecialMembers(*cls);
	// PA17: a virtual destructor occupies vtable slots, so an implicit
	// one must exist as a demandable definition even when no local code
	// destroys an object (the emitted vtable references its entries).
	if (cls->dtor_virtual && !cls->has_user_dtor && !cls->dtor_deleted)
		EnsureImplicitDtor(*cls);
	open_classes_.pop_back();
	if (open_classes_.empty())
		FlushDeferredBodies();
}

// --- special members --------------------------------------------------

void SemBinder::BindSpecialMember(const AstDecl& decl)
{
	ClassInfo* cls = OpenClass();
	const AstName* id = decl.declarator->IdName();
	if (!id || id->parts.empty())
		throw OutsideBoundary("special member name form");
	if (id->parts.size() > 1)
	{
		BindQualifiedSpecialMember(decl, *id);
		return;
	}
	if (!cls || current_ != cls->members)
		throw OutsideBoundary("out-of-class special member definition");
	const AstNamePart& part = id->parts[0];
	if (part.kind == NP_CONVERSION_FUNCTION)
	{
		BindConversionFunction(decl, *cls, part);
		return;
	}
	if (part.kind != NP_IDENTIFIER)
		throw OutsideBoundary("conversion function");
	if (part.identifier != cls->members->name)
		throw runtime_error("special member does not name its class");
	bool is_dtor = part.tilde;
	bool is_explicit = false;
	bool is_virtual = false;
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
	{
		ETokenType keyword = decl.member_specifiers[i].keyword;
		if (keyword == KW_EXPLICIT)
			is_explicit = true;
		else if (keyword == KW_VIRTUAL)
		{
			// PA17: only destructors may be virtual (12.1p4).
			if (!is_dtor)
				throw runtime_error("constructor declared virtual");
			is_virtual = true;
		}
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
		cls->dtor_user_declared = true;
		cls->dtor_access = current_access_;
		cls->dtor_deleted = deleted;
		cls->dtor_definition = defined ? &decl : 0;
		cls->dtor_unwind_no = composed.noexcept_simple;
		RecordVirtualDtor(*cls, is_virtual, composed, defined, defaulted,
		                  deleted);
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
	if (composed.has_override || composed.has_final)
		throw runtime_error("virt-specifier on a constructor");
	ClassCtor ctor;
	ctor.type = composed.type;
	for (size_t i = 0; i < composed.parameters.size(); i++)
		ctor.param_names.push_back(composed.parameters[i].name);
	ctor.access = current_access_;
	ctor.is_explicit = is_explicit;
	ctor.deleted = deleted;
	ctor.defaulted = defaulted;
	ctor.unwind_no = composed.noexcept_simple;
	ctor.definition = defined ? &decl : 0;
	for (size_t i = 0; i < composed.parameters.size(); i++)
		ctor.defaults.push_back(composed.parameters[i].default_arg);
	ctor.kind = ClassifyCtorKind(cls->entity, ctor);
	if (ctor.kind == CK_COPY)
		cls->has_user_copy_ctor = true;
	else if (ctor.kind == CK_MOVE)
		cls->has_user_move_ctor = true;
	cls->ctors.push_back(ctor);
	// 8.5.1p1/12.1p5: user-provided or deleted constructors disqualify
	// aggregates; `= default` on the default constructor keeps the
	// class an aggregate (C++11 semantics pinned by the fixtures).
	if (!defaulted && !deleted)
	{
		cls->is_aggregate = false;
		cls->has_user_ctor = true;
	}
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

// A conversion-function member declaration (12.3.2): bound in the
// class scope under its canonical "operator <type>" name, recorded in
// the class's conversion set, and (when defined in-class) deferred
// like other member bodies.
void SemBinder::BindConversionFunction(const AstDecl& decl, ClassInfo& cls,
                                       const AstNamePart& part)
{
	bool is_explicit = false;
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
	{
		ETokenType keyword = decl.member_specifiers[i].keyword;
		if (keyword == KW_EXPLICIT)
			is_explicit = true;
		else if (keyword == KW_VIRTUAL)
			throw OutsideBoundary("virtual conversion function");
	}
	TypePtr result = builder_.ResolveTypeId(*part.conversion_type);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		decl.declarator.get(), result);
	if (composed.type->kind != TK_FUNCTION ||
	    !composed.type->parameters.empty() || composed.type->variadic)
		throw runtime_error("conversion function takes no parameters");
	bool deleted = decl.special_init &&
		decl.special_init->kind == INIT_DELETE;
	bool defined = decl.kind == DK_SPECIAL_MEMBER_DEFINITION;
	string name = "operator " + DescribeType(result);
	ScopeBinding& binding = BindFunctionName(name, composed.type, false);
	RecordFunctionFacts(binding, composed, deleted, 0, defined);
	ClassConversion conv;
	conv.name = name;
	conv.result = result;
	conv.type = composed.type;
	conv.is_explicit = is_explicit;
	conv.access = current_access_;
	cls.conversions.push_back(conv);
	if (!defined)
		return;
	DeferredBody body;
	body.decl = &decl;
	body.composed = composed;
	body.name = name;
	body.fn_scope = MakeSpecialMemberScope(name, composed, cls);
	body.declaring = cls.members;
	body.cls = &cls;
	deferred_bodies_.push_back(body);
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

// An out-of-class constructor definition with a qualified name
// (`Outer::Buffer::Buffer(Token) {}`): the declaration must already
// exist in the named class; the body analyzes against that class.
// An out-of-class conversion-function definition: the
// conversion-type-id resolves in the class's scope.
void SemBinder::BindQualifiedConversionFunction(const AstDecl& decl,
                                                const AstNamePart& part,
                                                Scope* declaring,
                                                ClassInfo& cls)
{
	Scope* saved = current_;
	current_ = declaring;
	TypePtr result;
	DeclaratorInfo composed;
	try
	{
		result = builder_.ResolveTypeId(*part.conversion_type);
		composed = builder_.ComposeDeclarator(
			decl.declarator.get(), result);
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	string name = "operator " + DescribeType(result);
	if (!FindOwnBinding(*declaring, name))
		throw runtime_error("conversion definition matches no "
		                    "declaration");
	DeferredBody body;
	body.decl = &decl;
	body.composed = composed;
	body.name = name;
	body.fn_scope = MakeSpecialMemberScope(name, composed, cls);
	body.declaring = declaring;
	body.cls = &cls;
	body.out_of_class = true;
	AnalyzeDeferredBody(body);
}

void SemBinder::BindQualifiedSpecialMember(const AstDecl& decl,
                                           const AstName& id)
{
	bool defaulted = decl.kind == DK_SPECIAL_MEMBER_DECLARATION &&
		decl.special_init && decl.special_init->kind == INIT_DEFAULT;
	if (decl.kind != DK_SPECIAL_MEMBER_DEFINITION && !defaulted)
		throw OutsideBoundary("qualified special member declaration");
	Scope* declaring = ResolvePrefixScope(id);
	if (declaring->kind != SCOPE_CLASS)
		throw OutsideBoundary("qualified special member scope");
	// 7.1.2p5: `virtual` appears only on the in-class declaration; the
	// out-of-class definition cannot respell it.
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
		if (decl.member_specifiers[i].keyword == KW_VIRTUAL)
			throw runtime_error("virtual on an out-of-class special "
			                    "member definition");
	const AstNamePart& part = id.parts.back();
	const NamedTypeInfo* entity = model_.ScopeEntity(declaring);
	ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
	if (!cls)
		throw runtime_error("special member of an unknown class");
	if (part.kind == NP_CONVERSION_FUNCTION)
	{
		if (defaulted)
			throw runtime_error("defaulted conversion function");
		BindQualifiedConversionFunction(decl, part, declaring, *cls);
		return;
	}
	if (part.kind != NP_IDENTIFIER)
		throw OutsideBoundary("qualified special member name");
	if (part.identifier != declaring->name)
		throw runtime_error("special member does not name its class");
	if (part.tilde)
	{
		// Out-of-class destructor definition or `= default`.
		if (!cls->dtor_user_declared)
			throw runtime_error("destructor definition matches no "
			                    "declaration");
		if (cls->dtor_definition)
			throw runtime_error("redefinition of destructor");
		if (defaulted)
		{
			// 8.4.3: defaulted outside the class behaves implicitly
			// but emits as a source-owned strong definition.
			cls->has_user_dtor = false;
			InvalidateClassFacts();
			EnsureImplicitDtor(*cls, true);
			return;
		}
		DeferredBody body;
		body.decl = &decl;
		body.composed.type = MakeFunctionType(
			MakeFundamentalType(FT_VOID), vector<TypePtr>(), false);
		body.name = "~" + declaring->name;
		body.fn_scope = MakeSpecialMemberScope(body.name,
		                                       body.composed, *cls);
		body.declaring = declaring;
		body.cls = cls;
		body.out_of_class = true;
		cls->dtor_definition = &decl;
		cls->has_user_dtor = true;
		InvalidateClassFacts();
		// PA17: an out-of-class destructor definition anchors the
		// vtable when the destructor is the class's key function.
		if (cls->is_polymorphic && cls->key_is_dtor)
			cls->key_defined_in_tu = true;
		AnalyzeDeferredBody(body);
		return;
	}
	// Parameters resolve against the class's lexical scope.
	Scope* saved = current_;
	current_ = declaring;
	DeclaratorInfo composed;
	try
	{
		composed = builder_.ComposeDeclarator(
			decl.declarator.get(), MakeFundamentalType(FT_VOID));
	}
	catch (...)
	{
		current_ = saved;
		throw;
	}
	current_ = saved;
	int index = ClassCtorIndex(*cls, composed.type);
	if (index < 0)
		throw runtime_error("constructor definition matches no "
		                    "declaration");
	if (cls->ctors[index].definition)
		throw runtime_error("redefinition of constructor");
	if (defaulted)
	{
		ClassCtor& ctor = cls->ctors[index];
		if (ctor.kind == CK_ORDINARY && !ctor.type->parameters.empty())
			throw runtime_error("defaulted non-special constructor");
		ctor.defaulted = true;
		ctor.defaulted_outside = true;
		RecomputeUserCtorFact(*cls);
		if (ctor.kind == CK_ORDINARY)
			EnsureImplicitDefaultCtor(*cls, true);
		else
			EnsureSpecialCtor(*cls, index, true);
		return;
	}
	cls->ctors[index].definition = &decl;
	InvalidateClassFacts();
	DeferredBody body;
	body.decl = &decl;
	body.composed = composed;
	body.name = declaring->name;
	body.fn_scope = MakeSpecialMemberScope(body.name, composed, *cls);
	body.declaring = declaring;
	body.cls = cls;
	body.out_of_class = true;  // source-owned: both entries print
	AnalyzeDeferredBody(body);
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
			cls->friend_classes.push_back(specs.type->named);
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
	// PA17: defining the class's key function anchors its vtable in
	// this translation unit (emitted strong by the lowering).
	if (cls->is_polymorphic && !cls->key_is_dtor &&
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
	for (size_t i = 0; i < body.composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = body.composed.parameters[i].name;
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
	if (body.out_of_class && special == SF_NONE)
		AppendItem(std::move(item));
	else
	{
		if (body.out_of_class)
			// A source-owned constructor prints unconditionally.
			node->inline_def = false;
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

void SemBinder::BindInheritingConstructors(Scope* base_scope)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw runtime_error("using Base::Base outside a class");
	if (!cls->base || cls->base->members != base_scope)
		throw runtime_error("using Base::Base does not name the direct "
		                    "base");
	bool any_inherited = false;
	for (size_t i = 0; i < cls->base->ctors.size(); i++)
	{
		// 12.9p3: copy/move constructors of the base are not inherited.
		if (cls->base->ctors[i].kind != CK_ORDINARY)
			continue;
		ClassCtor ctor = cls->base->ctors[i];
		ctor.definition = 0;
		ctor.inherited_base = cls->base->entity;
		ctor.inherited_built = false;
		cls->ctors.push_back(ctor);
		any_inherited = true;
	}
	cls->has_user_ctor = any_inherited || cls->has_user_ctor;
	cls->is_aggregate = false;
}

// The forwarding definition of an inherited constructor (12.9p8): the
// declared parameters pass through to the base constructor; members
// still default-initialize.
void SemBinder::EnsureInheritedCtor(const ClassInfo& cls_in, int index)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	ClassCtor& ctor = cls.ctors[index];
	if (ctor.inherited_built)
		return;
	ctor.inherited_built = true;
	const ClassInfo* base = unit_.classes.Find(ctor.inherited_base);
	const string& base_name = cls.members->name;
	Scope* fn_scope =
		model_.CreateScope(SCOPE_FUNCTION, base_name, cls.members);
	DeferredBody body;
	body.name = base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.composed.type = ctor.type;
	for (size_t i = 0; i < ctor.type->parameters.size(); i++)
	{
		ParameterInfo parameter;
		parameter.name = i < ctor.param_names.size()
			? ctor.param_names[i] : "";
		parameter.type = ctor.type->parameters[i];
		body.composed.parameters.push_back(parameter);
		if (parameter.name.empty())
			continue;
		ScopeBinding param_binding;
		param_binding.kind = SB_PARAMETER;
		param_binding.name = parameter.name;
		param_binding.type = parameter.type;
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
	int base_index = base ? ClassCtorIndex(*base, ctor.type) : -1;
	vector<SemNodePtr> args;
	for (size_t i = 0; i < body.composed.parameters.size(); i++)
	{
		SemNodePtr arg = MakeSemNode(SN_ID_EXPRESSION);
		arg->name = body.composed.parameters[i].name;
		arg->type = body.composed.parameters[i].type;
		arg->category = VC_LVALUE;
		arg->entity_scope = fn_scope;
		arg->entity_name = arg->name;
		args.push_back(std::move(arg));
	}
	node->children.push_back(MakeConstructorCall(
		*base, base_index, true, ThisBaseAddress(cls), std::move(args)));
	vector<SemNodePtr> actions;
	if (cls.is_polymorphic)
		actions.push_back(MakeVPointerStore(cls));
	for (size_t i = 0; i < cls.fields.size(); i++)
		if (!cls.fields[i].name.empty() || !cls.fields[i].is_bit_field)
			AppendFieldDefaultInit(cls, cls.fields[i], actions);
	for (size_t i = 0; i < actions.size(); i++)
		node->children.push_back(std::move(actions[i]));
	current_ = saved_scope;
	method_ = saved_method;
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
                                  const string& what,
                                  const NamedTypeInfo* naming)
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
			if (owner_cls->friend_classes[j] == contexts[i]->entity)
				return;
	}
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < owner_cls->friend_functions.size(); i++)
			if (owner_cls->friend_functions[i].first == method_.fn_owner &&
			    owner_cls->friend_functions[i].second == method_.fn_name)
				return;
	// 11.2p5/11.4: a protected member of a base class is also
	// accessible to members and friends of any class P on the object
	// expression's derivation path to the owner; the naming class is
	// derived from P by construction, satisfying the 11.4 object-type
	// restriction.
	if (access == MA_PROTECTED && naming)
		for (const ClassInfo* p = unit_.classes.Find(naming);
		     p && p->members != owner; p = p->base)
		{
			for (size_t i = 0; i < contexts.size(); i++)
				for (size_t j = 0; j < p->friend_classes.size(); j++)
					if (p->friend_classes[j] == contexts[i]->entity)
						return;
			if (!method_.fn_name.empty())
				for (size_t i = 0; i < p->friend_functions.size(); i++)
					if (p->friend_functions[i].first ==
					        method_.fn_owner &&
					    p->friend_functions[i].second ==
					        method_.fn_name)
						return;
		}
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
			throw NoViableOverloadError(
				string("no matching constructor for ") + what);
		return -1;
	}
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	vector<size_t> positions;
	bool any_default_form = false;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		if (cls.ctors[i].ignore_in_overload)
			continue;
		size_t required = cls.ctors[i].type->parameters.size();
		const vector<const AstExpr*>& defaults = cls.ctors[i].defaults;
		while (required > 0 && required <= defaults.size() &&
		       defaults[required - 1])
			required--;
		if (required == 0)
			any_default_form = true;
		candidates.push_back(cls.ctors[i].type);
		min_arity.push_back(required);
		positions.push_back(i);
	}
	// Default-initialization of a class whose only constructor entries
	// are the implicitly declared copy/move members uses the implicit
	// default constructor.
	if (args.empty() && !cls.has_user_ctor && !any_default_form)
		return -1;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	vector<ImplicitConversion> conversions;
	size_t winner = positions[SelectBestOverload(candidates, sources,
	                                             conversions, &min_arity)];
	const ClassCtor& ctor = cls.ctors[winner];
	if (ctor.deleted)
		throw runtime_error(string("use of deleted constructor for ") +
		                    what);
	if (copy_init && ctor.is_explicit)
		throw runtime_error(string("explicit constructor selected by "
		                           "copy-initialization for ") + what);
	CheckMemberAccess(cls.members, ctor.access, "constructor");
	const TypePtr& fn = ctor.type;
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
	bool trivial_transfer = false;
	if (ctor_index < 0)
	{
		EnsureImplicitDefaultCtor(cls);
		ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
		                             vector<TypePtr>(), false);
		callee_unwind_no = cls.implicit_ctor_unwind_no;
	}
	else
	{
		const ClassCtor& selected = cls.ctors[ctor_index];
		ctor_type = selected.type;
		callee_unwind_no = selected.unwind_no;
		if (selected.inherited_base)
			EnsureInheritedCtor(cls, ctor_index);
		else if ((selected.kind == CK_COPY || selected.kind == CK_MOVE) &&
		         !selected.definition &&
		         (selected.implicit || selected.defaulted))
		{
			// An implicit/defaulted copy or move constructor: a trivial
			// one lowers as a raw object copy; otherwise the field-wise
			// definition synthesizes on first demand.
			trivial_transfer = selected.kind == CK_COPY
				? ClassHasTrivialCopyCtor(cls)
				: ClassHasTrivialMoveCtor(cls);
			if (trivial_transfer)
				callee_unwind_no = true;
			else
			{
				EnsureSpecialCtor(cls, ctor_index);
				callee_unwind_no = cls.ctors[ctor_index].built_unwind_no;
			}
		}
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
	if (trivial_transfer)
	{
		action->trivial_copy = true;
		action->type = MakeNamedType(TK_CLASS, cls.entity);
	}
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
	if (address)
	{
		call->children.push_back(std::move(address));
		action->ctor_addressed = true;
	}
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
	if (address)
		call->children.push_back(std::move(address));
	action->children.push_back(std::move(call));
	return action;
}
