#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 class-side machinery of the semantic binder: ClassInfo
// recording and layout, base clauses, special members, friends,
// bit-fields, and the deferred analysis of in-class member-function
// bodies (9.2p2: the class is complete within them).

// Whether an out-of-class member definition spells `inline` (7.1.2p4:
// weak, on-demand emission like an in-class definition). Shared with
// the member-body unit (sem_member_body.cpp).
bool DeclSpellsInline(const AstDecl& decl)
{
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
		if (decl.member_specifiers[i].keyword == KW_INLINE)
			return true;
	for (size_t i = 0; i < decl.specifiers.size(); i++)
		if (decl.specifiers[i].kind == SPEC_KEYWORD &&
		    decl.specifiers[i].keyword == KW_INLINE)
			return true;
	return false;
}

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
	cls.is_final = decl.class_final;
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

// PA20 9.4.2: a static data member with an in-class initializer
// evaluates into the constant store, so class-scope sizeof, member
// constant expressions, and the eventual storage definition all read
// the same typed value. Object-valued (array or class) members also
// complete their bound; non-integral scalars (floats, pointers) gain
// their only constant identity here (the integral fast path records
// `has_value` before this runs). The analyzed initializer persists in
// `member_image_inits_` for the storage definition.
void SemBinder::RecordStaticMemberObject(ScopeBinding& binding,
                                         const AstInitializer* init,
                                         const DeclSpecifierInfo& specs)
{
	if (!init || binding.has_value || InAbstractTemplateContext())
		return;
	if (IsReferenceType(binding.type))
		return;
	// Only const objects have compile-time values; a mutable static
	// member must never enter the constant store.
	bool is_const = false;
	bool is_volatile = false;
	TopCv(binding.type, is_const, is_volatile);
	if (!specs.is_constexpr && (!is_const || is_volatile))
		return;
	// A deferred nested member-class definition (14.7.1p1) completes
	// before the elements analyze.
	TypePtr element = RemoveTopCv(binding.type);
	while (element->kind == TK_ARRAY)
		element = RemoveTopCv(element->target);
	if (element->kind == TK_CLASS)
		EnsureTypeCompleteness(element->named);
	SemNodePtr holder = MakeSemNode(SN_VARIABLE);
	holder->name = binding.name;
	holder->type = binding.type;
	holder->entity_scope = binding.owner;
	holder->entity_name = binding.name;
	try
	{
		AttachObjectLifetime(*holder, binding, init, specs);
	}
	catch (const std::exception&)
	{
		return;  // outside the analyzed initializer subset
	}
	try
	{
		engine_.EvaluateVariableInit(*holder, binding.type,
		                             binding.owner, binding.name);
		// The analyzed actions persist for the storage definition's
		// odr-use tracking (3.2p3 over dropped constant init).
		member_image_inits_[std::pair<const void*, string>(
			binding.owner, binding.name)] = std::move(holder);
	}
	catch (const std::exception&)
	{
		// Not constant: uses in constant expressions diagnose there.
	}
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
		RecordStaticMemberObject(binding, init, specs);
		return;  // static data members are not fields
	}
	if (in_bit_field_)
		return;  // the bit-field path lays its own rows
	// PA18: a field requires its class type complete (deferred nested
	// member-class definitions instantiate here).
	TypePtr field_bare = binding.type;
	while (field_bare->kind == TK_ARRAY)
		field_bare = field_bare->target;
	if (field_bare->kind == TK_CLASS)
		EnsureTypeCompleteness(field_bare->named);
	ClassField field;
	field.name = binding.name;
	field.type = binding.type;
	field.is_mutable = specs.is_mutable;
	field.no_unique_address = specs.no_unique_address;
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

// The member-specifier keywords of one special-member declaration
// (12.1p4: only destructors may be virtual).
static void ReadSpecialMemberSpecifiers(SemBinder& binder,
                                        const AstDecl& decl, bool is_dtor,
                                        bool& is_explicit,
                                        bool& is_virtual)
{
	for (size_t i = 0; i < decl.member_specifiers.size(); i++)
	{
		const AstMemberSpecifier& spec = decl.member_specifiers[i];
		if (spec.keyword == KW_EXPLICIT)
			// A conditional `explicit(expr)` evaluates per
			// instantiation (SemBinder::ExplicitSpecifierValue).
			is_explicit = binder.ExplicitSpecifierValue(spec);
		else if (spec.keyword == KW_VIRTUAL)
		{
			if (!is_dtor)
				throw runtime_error("constructor declared virtual");
			is_virtual = true;
		}
		else if (spec.keyword == KW_STATIC || spec.keyword == KW_FRIEND)
			throw runtime_error("invalid specifier on a special member");
	}
}

// C++20 explicit(constant-expression) (PA34 hosted concession): the
// condition contextually converts to bool per instantiation.
bool SemBinder::ExplicitSpecifierValue(const AstMemberSpecifier& spec)
{
	if (!spec.explicit_expr)
		return true;
	ConstValue value;
	if (!TryEvaluateConstant(*spec.explicit_expr, value) &&
	    !TryFullConstant(*spec.explicit_expr, value))
		throw runtime_error("explicit specifier condition is not a "
		                    "constant expression");
	return ConstValueIsNonZero(value);
}

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
	// Inside an instantiated specialization the declared name is the
	// template's ("Box") while the member scope carries the
	// specialization spelling ("Box<int>").
	if (part.identifier != cls->members->name &&
	    !(cls->entity && cls->entity->spec_template &&
	      part.identifier == cls->entity->spec_template->name))
		throw runtime_error("special member does not name its class");
	bool is_dtor = part.tilde;
	bool is_explicit = false;
	bool is_virtual = false;
	ReadSpecialMemberSpecifiers(*this, decl, is_dtor, is_explicit,
	                            is_virtual);
	// Composing the declarator over void yields exactly the
	// constructor/destructor function type over the declared parameters.
	// A pack-expanded parameter records its umbrella binding for the
	// member scope below; a stale record must not leak in.
	last_pack_param_ = PackParamRecord();
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
		if (decl.declarator)
			cls->dtor_abi_tags = decl.declarator->abi_tags;
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
	if (decl.declarator)
		ctor.abi_tags = decl.declarator->abi_tags;
	ctor.access = current_access_;
	ctor.is_explicit = is_explicit;
	ctor.deleted = deleted;
	ctor.defaulted = defaulted;
	ctor.unwind_no = composed.noexcept_simple;
	ctor.noexcept_decl = composed.noexcept_simple;
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
			is_explicit =
				ExplicitSpecifierValue(decl.member_specifiers[i]);
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
	if (decl.kind == DK_SPECIAL_MEMBER_DEFINITION)
		conv.decl = &decl;
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
	fn_scope->fn_type = composed.type;
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
	// PA34: the composed declarator's pack-expanded parameter (if
	// any) publishes its umbrella binding, so `a...` expansions in
	// the ctor-initializer and body read it.
	BindCapturedPackParameter(fn_scope);
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
	BindQualifiedSpecialMemberInner(decl, id, defaulted);
}

// Out-of-class destructor definition or `= default`.
void SemBinder::BindQualifiedDestructor(const AstDecl& decl,
                                        Scope* declaring, ClassInfo& cls,
                                        bool defaulted)
{
	if (!cls.dtor_user_declared)
		throw runtime_error("destructor definition matches no "
		                    "declaration");
	if (cls.dtor_definition)
		throw runtime_error("redefinition of destructor");
	if (defaulted)
	{
		// 8.4.3: defaulted outside the class behaves implicitly
		// but emits as a source-owned strong definition.
		cls.has_user_dtor = false;
		InvalidateClassFacts();
		EnsureImplicitDtor(cls, true);
		return;
	}
	DeferredBody body;
	body.decl = &decl;
	body.composed.type = MakeFunctionType(
		MakeFundamentalType(FT_VOID), vector<TypePtr>(), false);
	body.name = "~" + declaring->name;
	body.fn_scope = MakeSpecialMemberScope(body.name, body.composed,
	                                       cls);
	body.declaring = declaring;
	body.cls = &cls;
	body.out_of_class = true;
	cls.dtor_definition = &decl;
	cls.has_user_dtor = true;
	InvalidateClassFacts();
	// PA17: an out-of-class destructor definition anchors the
	// vtable when the destructor is the class's key function.
	if (cls.is_polymorphic && cls.key_is_dtor)
		cls.key_defined_in_tu = true;
	AnalyzeDeferredBody(body);
}

void SemBinder::BindQualifiedSpecialMemberInner(const AstDecl& decl,
                                                const AstName& id,
                                                bool defaulted)
{
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
	// An instantiated member definition spells the pattern name
	// ("Box") while the specialization's member scope is renamed to
	// the specialization spelling ("Box<int>").
	bool names_class = part.identifier == declaring->name ||
		(entity->spec_template &&
		 part.identifier == entity->spec_template->name);
	if (!names_class)
		throw runtime_error("special member does not name its class");
	if (part.tilde)
	{
		BindQualifiedDestructor(decl, declaring, *cls, defaulted);
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
	// Source-owned: both entries print; a spelled-inline definition
	// prints weak but still prints.
	body.out_of_class = true;
	body.spelled_inline = DeclSpellsInline(decl);
	AnalyzeDeferredBody(body);
}

// --- friends ------------------------------------------------------------

// An elaborated template-id specifier (`friend class BitRef<Bitset>;`,
// `struct X<int>` as a type specifier) names the specialization
// through the instantiation seam (3.4.4 with 14.2), filling defaulted
// arguments; the record stays dormant (14.7.1p1).
TypePtr SemBinder::BindNestedTypeSpecifier(const AstDecl& decl)
{
	if (decl.kind == DK_CLASS_FORWARD && decl.has_name &&
	    !decl.class_name.parts.empty() &&
	    decl.class_name.parts.back().kind == NP_TEMPLATE_ID)
	{
		const ScopeBinding* binding = ResolveTerminal(decl.class_name,
		                                              SLF_ANY);
		if (binding && binding->kind == SB_TYPE && binding->type &&
		    RemoveTopCv(binding->type)->kind == TK_CLASS)
			return binding->type;
	}
	return DeclBinder::BindNestedTypeSpecifier(decl);
}

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
		{
			// PA21 11.3p3: `friend typename C::self;` (a dependent
			// type naming a class at instantiation) grants that class.
			for (size_t i = 0; i < decl.specifiers.size(); i++)
			{
				if (decl.specifiers[i].kind != SPEC_TYPE_NAME)
					continue;
				try
				{
					TypePtr named =
						ResolveTypeName(decl.specifiers[i].name);
					if (named &&
					    RemoveTopCv(named)->kind == TK_CLASS)
						cls->friend_classes.push_back(
							RemoveTopCv(named)->named);
				}
				catch (const std::exception&)
				{
					// A non-type friend specifier declares nothing.
				}
			}
			return;  // friend of a non-class type declares nothing
		}
		// A plain `friend class X;` declares X in the enclosing
		// namespace (3.4.4/7.3.1.2p3); a template-id friend only
		// references an existing specialization, and its arguments
		// (the injected-class-name included) resolve from here.
		const AstDecl* nested = 0;
		for (size_t i = 0; i < decl.specifiers.size(); i++)
			if (decl.specifiers[i].kind == SPEC_NESTED_DECL)
				nested = decl.specifiers[i].nested_decl.get();
		bool reference_only = nested &&
			nested->kind == DK_CLASS_FORWARD && nested->has_name &&
			!nested->class_name.parts.empty() &&
			nested->class_name.parts.back().kind == NP_TEMPLATE_ID;
		Scope* saved = current_;
		if (!reference_only)
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
	const AstNamePart& terminal = composed.id->parts.back();
	if (!qualified && decl.kind != DK_FUNCTION &&
	    (terminal.kind == NP_TEMPLATE_ID ||
	     terminal.operator_template_id || !terminal.arguments.empty()))
	{
		BindTemplateIdFriend(target, name, composed.type);
		return;
	}
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
		fn_scope->fn_type = composed.type;
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

void SemBinder::BindInheritingConstructors(Scope* base_scope)
{
	ClassInfo* cls = OpenClass();
	if (!cls || current_ != cls->members)
		throw runtime_error("using Base::Base outside a class");
	// PA26: the named class may be any direct base, not only the first.
	const ClassInfo* named = 0;
	for (size_t b = 0; b < cls->direct_bases.size(); b++)
		if (cls->direct_bases[b].cls->members == base_scope)
		{
			named = cls->direct_bases[b].cls;
			break;
		}
	if (!named)
		throw runtime_error("using Base::Base does not name a direct "
		                    "base");
	bool any_inherited = false;
	for (size_t i = 0; i < named->ctors.size(); i++)
	{
		// 12.9p3: copy/move constructors of the base are not inherited.
		if (named->ctors[i].kind != CK_ORDINARY)
			continue;
		ClassCtor ctor = named->ctors[i];
		ctor.definition = 0;
		ctor.inherited_base = named->entity;
		ctor.inherited_built = false;
		cls->ctors.push_back(ctor);
		any_inherited = true;
	}
	// 12.9 with member templates: base constructor templates inherit
	// as constructor templates; a deduced selection synthesizes a
	// forwarding entry (EnsureCtorTemplateEntry marks it inherited by
	// the template's owning class).
	for (size_t i = 0; i < named->ctor_templates.size(); i++)
	{
		cls->ctor_templates.push_back(named->ctor_templates[i]);
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
		// The forwarding body names every parameter (the base
		// declaration may leave them unnamed).
		if (parameter.name.empty())
			parameter.name = "__param" + std::to_string(i + 1);
		parameter.type = ctor.type->parameters[i];
		body.composed.parameters.push_back(parameter);
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
	// An inherited constructor-template selection synthesizes the
	// base's own entry (and instantiates its body) on first forward.
	if (base_index < 0 && ctor.tmpl_spec && base)
	{
		ClassInfo& mutable_base = unit_.classes.Create(base->entity);
		base_index = EnsureCtorTemplateEntry(mutable_base,
		                                     ctor.tmpl_spec);
		if (base_index >= 0)
			InstantiateCtorTemplateBody(mutable_base, base_index);
	}
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
	// 12.9p8: the named base takes the forwarded call in its
	// declaration-order slot; every other direct base
	// default-initializes.
	size_t inherited_at = cls.direct_bases.size();
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
		if (cls.direct_bases[b].cls->entity == ctor.inherited_base)
		{
			inherited_at = b;
			break;
		}
	if (inherited_at == cls.direct_bases.size())
		throw runtime_error("inherited constructor names no direct base");
	vector<SemNodePtr> actions;
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		if (b == inherited_at)
			actions.push_back(MakeConstructorCall(
				*base, base_index, true, ThisBaseAddress(cls, b),
				std::move(args)));
		else
			AppendOneBaseDefaultInit(cls, b, actions, false);
	}
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
	// PA24: inside a lambda body, `this` means the enclosing member
	// function's object; a captureless lambda has none (frame.cls
	// null leaves the placeholder-free null result and the ordinary
	// diagnostics fire).
	if (LambdaFrame* frame = ActiveLambdaFrame())
		return frame->cls ? frame->enclosing_this : TypePtr();
	return method_.this_type;
}

// 5.1.1p3: within a member function declarator (notably a
// trailing-return decltype), members of the class resolve through an
// implicit `this` of the (possibly still-open) class.
void SemBinder::OnMemberSignatureBegin(Scope* class_scope)
{
	signature_contexts_.push_back(method_);
	method_ = MethodContext();
	const NamedTypeInfo* entity = model_.ScopeEntity(class_scope);
	if (!entity)
		return;
	method_.cls = unit_.classes.Find(entity);
	method_.fn_owner = class_scope;
	method_.this_type = MakePointerType(MakeNamedType(TK_CLASS, entity),
	                                    false, false);
}

void SemBinder::OnMemberSignatureEnd()
{
	method_ = signature_contexts_.back();
	signature_contexts_.pop_back();
}

void SemBinder::CheckMemberAccess(const Scope* owner, EMemberAccess access,
                                  const string& what,
                                  const NamedTypeInfo* naming)
{
	const NamedTypeInfo* owner_entity = model_.ScopeEntity(owner);
	if (access == MA_PUBLIC &&
	    (!naming || naming == owner_entity))
		return;
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
	// 11.2p1/p4: each non-public base edge on the naming class's
	// derivation path to the owner restricts access further - a private
	// base's members are reachable only inside the deriving class (or
	// its friends), a protected base's also inside classes derived from
	// it.
	if (naming && owner_entity && naming != owner_entity)
	{
		vector<ClassBaseEdge> edges;
		if (BaseAccessPath(naming, owner_entity, edges))
			for (size_t e = 0; e < edges.size(); e++)
				CheckBaseEdgeAccess(edges[e], contexts, what);
	}
	if (access == MA_PUBLIC)
		return;
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i]->members == owner)
			return;
		if (access == MA_PROTECTED)
		{
			// The context may derive from the owner through any path
			// of the base DAG (PA26).
			vector<const ClassInfo*> chain;
			CollectClassAndBases(contexts[i], chain);
			for (size_t j = 0; j < chain.size(); j++)
				if (chain[j]->members == owner)
					return;
		}
		// A friend class's members access everything (11.3); a friend
		// class template's grant covers every specialization.
		for (size_t j = 0; j < owner_cls->friend_classes.size(); j++)
			if (FriendClassMatches(owner_cls->friend_classes[j],
			                       contexts[i]->entity))
				return;
	}
	// A friend captured inside an enclosing instantiation declares
	// through the specialization's alias scope; friendship recorded
	// its home namespace.
	const Scope* fn_home = method_.fn_owner;
	while (fn_home && fn_home->kind == SCOPE_TEMPLATE_PARAMS)
		fn_home = fn_home->parent;
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < owner_cls->friend_functions.size(); i++)
			if (owner_cls->friend_functions[i].first == fn_home &&
			    (owner_cls->friend_functions[i].second ==
			         method_.fn_name ||
			     (!method_.fn_template_name.empty() &&
			      owner_cls->friend_functions[i].second ==
			          method_.fn_template_name)))
				return;
	// 11.2p5/11.4: a protected member of a base class is also
	// accessible to members and friends of any class P on the object
	// expression's derivation path to the owner; the naming class is
	// derived from P by construction, satisfying the 11.4 object-type
	// restriction. The path may run through any branch of the base DAG
	// (PA26).
	if (access == MA_PROTECTED && naming)
	{
		vector<const ClassInfo*> path;
		CollectClassAndBases(unit_.classes.Find(naming), path);
		for (size_t k = 0; k < path.size(); k++)
		{
			const ClassInfo* p = path[k];
			if (p->members == owner ||
			    !DerivedFromWithExtrasLinked(p->entity, owner_entity))
				continue;
			for (size_t i = 0; i < contexts.size(); i++)
				for (size_t j = 0; j < p->friend_classes.size(); j++)
					if (FriendClassMatches(p->friend_classes[j],
					                       contexts[i]->entity))
						return;
			if (!method_.fn_name.empty())
				for (size_t i = 0; i < p->friend_functions.size(); i++)
					if (p->friend_functions[i].first == fn_home &&
					    p->friend_functions[i].second ==
					        method_.fn_name)
						return;
		}
	}
	throw runtime_error(what + " is inaccessible in this context");
}

// One base-specifier edge of the naming class's derivation path: a
// non-public edge admits the deriving class itself, its friends, and
// (protected) classes derived from it (11.2p1/p4).
void SemBinder::CheckBaseEdgeAccess(const ClassBaseEdge& edge,
                                    const vector<const ClassInfo*>& contexts,
                                    const string& what)
{
	EMemberAccess edge_access =
		edge.derived->direct_bases[edge.base_index].access;
	if (edge_access == MA_PUBLIC)
		return;
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i] == edge.derived)
			return;
		if (edge_access == MA_PROTECTED &&
		    DerivedFromWithExtrasLinked(contexts[i]->entity,
		                                edge.derived->entity))
			return;
		for (size_t j = 0; j < edge.derived->friend_classes.size(); j++)
			if (FriendClassMatches(edge.derived->friend_classes[j],
			                       contexts[i]->entity))
				return;
	}
	const Scope* fn_home = method_.fn_owner;
	while (fn_home && fn_home->kind == SCOPE_TEMPLATE_PARAMS)
		fn_home = fn_home->parent;
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < edge.derived->friend_functions.size(); i++)
			if (edge.derived->friend_functions[i].first == fn_home &&
			    edge.derived->friend_functions[i].second == method_.fn_name)
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

SemNodePtr SemBinder::ThisBaseAddress(const ClassInfo& cls,
                                      size_t base_index)
{
	const ClassInfo* base = cls.base;
	unsigned long long offset = 0;
	if (base_index < cls.direct_bases.size())
	{
		base = cls.direct_bases[base_index].cls;
		offset = cls.direct_bases[base_index].offset;
	}
	SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
	member->type = MakeNamedType(TK_CLASS, base->entity);
	member->category = VC_LVALUE;
	member->base_hops = 1;
	member->base_offset = offset;
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
		// 8.5.3p5: an lvalue binds any reference form; an xvalue binds
		// an rvalue reference or a const lvalue reference. A prvalue
		// initializer (temporary materialization) stays out of scope.
		bool xvalue_ok = field.type->kind == TK_RVALUE_REFERENCE ||
			field.type->target->is_const;
		if (value.category != VC_LVALUE &&
		    !(value.category == VC_XVALUE && xvalue_ok))
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

// An implicit/defaulted copy or move constructor: a trivial one
// lowers as a raw object copy; otherwise the field-wise definition
// synthesizes on first demand. The reference keeps the synthesized
// call for a move over enumeration members and for a class with a
// user-provided destructor - its own or a subobject's (their pinned
// shapes). Returns whether the call stays a raw transfer and updates
// the callee's unwind fact.
bool SemBinder::ClassifyTransferCtor(const ClassInfo& cls, int ctor_index,
                                     bool& callee_unwind_no)
{
	const ClassCtor& selected = cls.ctors[ctor_index];
	bool enum_member = false;
	for (size_t f = 0; f < cls.fields.size(); f++)
		if (RemoveTopCv(cls.fields[f].type)->kind == TK_ENUM)
			enum_member = true;
	bool user_dtor_transfer = cls.has_user_dtor ||
		(cls.base && cls.base->has_user_dtor);
	for (size_t f = 0; f < cls.fields.size(); f++)
	{
		const ClassInfo* member = SubobjectClass(cls.fields[f].type);
		if (member && member->has_user_dtor)
			user_dtor_transfer = true;
	}
	bool trivial_transfer = !user_dtor_transfer &&
		(selected.kind == CK_COPY
			 ? ClassHasTrivialCopyCtor(cls)
			 : ClassHasTrivialMoveCtor(cls) && !enum_member);
	if (trivial_transfer)
	{
		callee_unwind_no = true;
		// PA18: a user-defaulted trivial copy/move selected inside an
		// instantiated body still synthesizes its weak definition;
		// the call itself stays a raw copy.
		if (instantiating_ && selected.defaulted)
			EnsureSpecialCtor(cls, ctor_index);
	}
	else
	{
		EnsureSpecialCtor(cls, ctor_index);
		callee_unwind_no = cls.ctors[ctor_index].built_unwind_no;
	}
	return trivial_transfer;
}

SemNodePtr SemBinder::MakeConstructorCall(const ClassInfo& cls,
                                          int ctor_index, bool base_entry,
                                          SemNodePtr address,
                                          vector<SemNodePtr> args)
{
	TypePtr ctor_type;
	bool callee_unwind_no = false;
	bool trivial_transfer = false;
	// 5.3.7: a user-provided constructor's noexcept fact is its
	// declared specification; implicit/defaulted members report their
	// implicit (computed) specification.
	bool user_provided = false;
	bool declared_noexcept = false;
	bool computed_spec = false;
	bool have_computed_spec = false;
	if (ctor_index < 0)
	{
		EnsureImplicitDefaultCtor(cls);
		ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
		                             vector<TypePtr>(), false);
		callee_unwind_no = cls.implicit_ctor_unwind_no;
		computed_spec = cls.implicit_ctor_noexcept;
		have_computed_spec = true;
	}
	else
	{
		const ClassCtor& selected = cls.ctors[ctor_index];
		ctor_type = selected.type;
		callee_unwind_no = selected.unwind_no;
		user_provided = selected.definition != 0 && !selected.defaulted;
		declared_noexcept = selected.noexcept_decl;
		if (selected.kind == CK_ORDINARY && !selected.definition &&
		    selected.defaulted && selected.type->parameters.empty() &&
		    !cls.has_user_ctor)
		{
			// 12.1p6: an explicitly-defaulted default constructor is
			// implicitly defined when odr-used; the implicit body
			// serves it and carries the implicit exception fact.
			EnsureImplicitDefaultCtor(cls);
			callee_unwind_no = callee_unwind_no ||
				cls.implicit_ctor_unwind_no;
			computed_spec = declared_noexcept ||
				cls.implicit_ctor_noexcept;
			have_computed_spec = true;
		}
		if (selected.inherited_base)
			EnsureInheritedCtor(cls, ctor_index);
		else if (selected.tmpl_spec)
			// A conversion-selected constructor-template entry
			// instantiates its body at this first use, like a
			// construction-site selection.
			InstantiateCtorTemplateBody(const_cast<ClassInfo&>(cls),
			                            ctor_index);
		else if ((selected.kind == CK_COPY || selected.kind == CK_MOVE) &&
		         !selected.definition &&
		         (selected.implicit || selected.defaulted))
			trivial_transfer = ClassifyTransferCtor(cls, ctor_index,
			                                        callee_unwind_no);
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
	callee->noexcept_decl = user_provided
		? declared_noexcept
		: have_computed_spec ? computed_spec : callee_unwind_no;
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
