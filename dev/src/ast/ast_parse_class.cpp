#include "ast/ast_parser.h"

using std::string;
using std::move;

namespace {

AstDeclPtr MakeDecl(EDeclKind kind)
{
	return AstDeclPtr(new AstDecl(kind));
}

bool IsClassKey(ETokenType type)
{
	return type == KW_CLASS || type == KW_STRUCT || type == KW_UNION;
}

bool IsAccessKeyword(ETokenType type)
{
	return type == KW_PUBLIC || type == KW_PRIVATE || type == KW_PROTECTED;
}

bool IsMemberFunctionSpecifier(ETokenType type)
{
	switch (type)
	{
	case KW_INLINE: case KW_VIRTUAL: case KW_EXPLICIT:
	case KW_CONSTEXPR: case KW_FRIEND: case KW_STATIC:
		return true;
	default:
		return false;
	}
}

}  // namespace

// base-clause: : base-specifier (, base-specifier)*. Base names stay
// unresolved structured names per the handout.
bool AstParser::ParseBaseClause(AstDecl& decl)
{
	if (!MatchSimple(OP_COLON))
		return false;
	for (;;)
	{
		AstBaseSpecifier base;
		if (AtSimple(KW_VIRTUAL))
		{
			base.is_virtual = true;
			base.virtual_spelling = Peek().spelling;
			Advance();
		}
		if (Peek().kind == PTOK_SIMPLE && IsAccessKeyword(Peek().simple_type))
		{
			base.has_access = true;
			base.access = Peek().simple_type;
			base.access_spelling = Peek().spelling;
			Advance();
		}
		if (!base.is_virtual && AtSimple(KW_VIRTUAL))
		{
			base.is_virtual = true;
			base.virtual_spelling = Peek().spelling;
			Advance();
		}
		if (AtSimple(KW_DECLTYPE))
		{
			Advance();
			AstExprPtr expr;
			if (!MatchSimple(OP_LPAREN) || !(expr = ParseExpression()) ||
			    !MatchSimple(OP_RPAREN))
				return false;
			AstNamePart part;
			part.kind = NP_DECLTYPE;
			part.decltype_expr = move(expr);
			base.name.parts.push_back(move(part));
		}
		else if (!ParseQualifiedTypeName(base.name))
			return false;
		base.pack = MatchSimple(OP_DOTS);
		decl.bases.push_back(move(base));
		if (!MatchSimple(OP_COMMA))
			return true;
	}
}

// alignment-specifier operand: alignas(type-id) or
// alignas(constant-expression) (7.6.2), captured for the class layout.
void AstParser::ParseClassAdornments(AstDecl& decl)
{
	for (;;)
	{
		State state = Save();
		if (AtIdentifierSpelled("__attribute__"))
		{
			Advance();
			if (SkipBalancedParens())
				continue;
			Restore(state);
			return;
		}
		if (AtSimple(KW_ALIGNAS))
		{
			Advance();
			if (!MatchSimple(OP_LPAREN))
			{
				Restore(state);
				return;
			}
			State operand = Save();
			AstTypeIdPtr type;
			// GNU `__alignof(T)` is an expression operand even though
			// it parses as a type-id shape.
			if (!AtIdentifierSpelled("__alignof") &&
			    !AtIdentifierSpelled("__alignof__") &&
			    ParseTypeId(type) && MatchSimple(OP_RPAREN))
			{
				decl.align_types.push_back(move(type));
				continue;
			}
			Restore(operand);
			AstExprPtr expr = ParseAssignmentExpression();
			if (expr && MatchSimple(OP_RPAREN))
			{
				decl.align_exprs.push_back(move(expr));
				continue;
			}
			Restore(state);
			return;
		}
		return;
	}
}

// class-key adornments? class-name? base-clause? { class-member* }
AstDeclPtr AstParser::ParseClassSpecifier()
{
	State state = Save();
	if (Peek().kind != PTOK_SIMPLE || !IsClassKey(Peek().simple_type))
		return AstDeclPtr();
	AstDeclPtr decl = MakeDecl(DK_CLASS);
	decl->class_key = Peek().simple_type;
	decl->class_key_spelling = Peek().spelling;
	Advance();
	ParseClassAdornments(*decl);
	State name_state = Save();
	if (!ParseQualifiedTypeName(decl->class_name))
		Restore(name_state);
	else
		decl->has_name = true;
	if (decl->has_name && decl->class_name.IsPlainIdentifier())
		RegisterInDeclScope(decl->class_name.parts[0].identifier,
		                    NF_TYPE | TemplatedFlag());
	if (AtSimple(OP_COLON))
	{
		if (!ParseBaseClause(*decl))
		{
			Restore(state);
			return AstDeclPtr();
		}
	}
	if (!AtSimple(OP_LBRACE))
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (!ParseClassBody(*decl))
	{
		Restore(state);
		return AstDeclPtr();
	}
	// Specifier-only span; a standalone class-declaration is re-stamped
	// with its full span (trailing semicolon included) by the caller.
	decl->begin_token = state.pos;
	decl->end_token = pos_;
	return decl;
}

bool AstParser::ParseClassBody(AstDecl& decl)
{
	Advance();  // OP_LBRACE
	NameTable* table = 0;
	if (decl.has_name && !decl.class_name.parts.empty() &&
	    !decl.class_name.parts.back().identifier.empty())
		table = GetOrCreateChild(DeclScopeTable(),
		                         decl.class_name.parts.back().identifier);
	else
		table = NewTable();
	PushScope(table, false);
	while (!AtSimple(OP_RBRACE))
	{
		AstDeclPtr member = ParseMemberDeclaration();
		if (!member)
		{
			PopScope();
			return false;
		}
		decl.members.push_back(move(member));
	}
	PopScope();
	Advance();  // OP_RBRACE
	return true;
}

// class-key class-name with neither body nor semicolon: the
// elaborated-type-specifier form used inside specifier sequences
// (friend struct Y; sizeof(struct sigaction)).
AstDeclPtr AstParser::ParseElaboratedClass()
{
	State state = Save();
	if (Peek().kind != PTOK_SIMPLE || !IsClassKey(Peek().simple_type))
		return AstDeclPtr();
	AstDeclPtr decl = MakeDecl(DK_CLASS_FORWARD);
	decl->class_key = Peek().simple_type;
	decl->class_key_spelling = Peek().spelling;
	Advance();
	ParseClassAdornments(*decl);
	if (!ParseQualifiedTypeName(decl->class_name))
	{
		Restore(state);
		return AstDeclPtr();
	}
	decl->has_name = true;
	return decl;
}

// class-declaration: a class-specifier or forward declaration with
// its trailing semicolon (the dump shows the specifier node itself).
AstDeclPtr AstParser::ParseClassDeclaration()
{
	State state = Save();
	AstDeclPtr decl = ParseClassSpecifier();
	if (decl)
	{
		if (MatchSimple(OP_SEMICOLON))
			return decl;
		Restore(state);
		return AstDeclPtr();
	}
	decl = ParseElaboratedClass();
	if (decl && MatchSimple(OP_SEMICOLON))
	{
		if (decl->class_name.IsPlainIdentifier())
			RegisterInDeclScope(decl->class_name.parts[0].identifier,
			                    NF_TYPE);
		return decl;
	}
	Restore(state);
	return AstDeclPtr();
}

// enum-specifier: KW_ENUM enum-key? name? (: type-specifier-seq)?
// ({ enumerator-list? ,? })?; bodiless forms require a name.
AstDeclPtr AstParser::ParseEnumSpecifier()
{
	State state = Save();
	if (!MatchSimple(KW_ENUM))
		return AstDeclPtr();
	AstDeclPtr decl = MakeDecl(DK_ENUM);
	if (AtSimple(KW_CLASS) || AtSimple(KW_STRUCT))
	{
		decl->has_enum_key = true;
		decl->enum_key = Peek().simple_type;
		decl->enum_key_spelling = Peek().spelling;
		Advance();
	}
	if (AtIdentifier())
	{
		decl->name = Peek().spelling;
		Advance();
	}
	if (AtSimple(OP_COLON))
	{
		Advance();
		AstTypeIdPtr base(new AstTypeId());
		if (!ParseSpecifierSeq(base->specifiers, kTypeSpecifierSeq))
		{
			Restore(state);
			return AstDeclPtr();
		}
		decl->has_enum_base = true;
		decl->type = move(base);
	}
	if (AtSimple(OP_LBRACE))
	{
		Advance();
		decl->enum_body = true;
		while (!AtSimple(OP_RBRACE))
		{
			if (!AtIdentifier())
			{
				Restore(state);
				return AstDeclPtr();
			}
			AstEnumerator enumerator;
			enumerator.name = Peek().spelling;
			Advance();
			if (MatchSimple(OP_ASS))
			{
				enumerator.value = ParseAssignmentExpression();
				if (!enumerator.value)
				{
					Restore(state);
					return AstDeclPtr();
				}
			}
			Register(enumerator.name, NF_VALUE);
			decl->enumerators.push_back(move(enumerator));
			if (!MatchSimple(OP_COMMA))
				break;
		}
		if (!MatchSimple(OP_RBRACE))
		{
			Restore(state);
			return AstDeclPtr();
		}
	}
	else if (decl->name.empty())
	{
		Restore(state);
		return AstDeclPtr();
	}
	RegisterInDeclScope(decl->name, NF_TYPE);
	decl->begin_token = state.pos;
	decl->end_token = pos_;
	return decl;
}

AstDeclPtr AstParser::ParseEnumDeclaration()
{
	State state = Save();
	AstDeclPtr decl = ParseEnumSpecifier();
	if (!decl || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// special-member-name: TT_IDENTIFIER | ~TT_IDENTIFIER |
// conversion-function-id | operator-function-id, optionally qualified;
// a qualified name only allows the identifier, destructor, and
// conversion forms (so `C::operator+(...)` without a return type
// stays ill-formed).
bool AstParser::ParseSpecialMemberName(AstName& name)
{
	State state = Save();
	name.global_scope = MatchSimple(OP_COLON2);
	ParseNestedNameParts(name);
	bool qualified = !name.parts.empty();
	if (AtSimple(OP_COMPL) && AtIdentifier(1))
	{
		Advance();
		AstNamePart part;
		part.tilde = true;
		part.identifier = Peek().spelling;
		Advance();
		name.parts.push_back(move(part));
		return true;
	}
	if (AtSimple(KW_OPERATOR))
	{
		AstNamePart part;
		if (!ParseOperatorIdPart(part) ||
		    (qualified && part.kind != NP_CONVERSION_FUNCTION))
		{
			Restore(state);
			return false;
		}
		name.parts.push_back(move(part));
		return true;
	}
	if (AtIdentifier())
	{
		AstNamePart part;
		part.identifier = Peek().spelling;
		Advance();
		name.parts.push_back(move(part));
		return true;
	}
	Restore(state);
	return false;
}

// ctor-initializer: : mem-initializer (, mem-initializer)*
bool AstParser::ParseCtorInitializer(AstDecl& decl)
{
	if (!MatchSimple(OP_COLON))
		return false;
	for (;;)
	{
		AstMemInitializer init;
		if (AtSimple(KW_DECLTYPE))
		{
			Advance();
			AstExprPtr expr;
			if (!MatchSimple(OP_LPAREN) || !(expr = ParseExpression()) ||
			    !MatchSimple(OP_RPAREN))
				return false;
			AstNamePart part;
			part.kind = NP_DECLTYPE;
			part.decltype_expr = move(expr);
			init.id.parts.push_back(move(part));
		}
		else if (!ParseQualifiedTypeName(init.id))
			return false;
		init.init = AstInitializerPtr(new AstInitializer());
		if (AtSimple(OP_LPAREN))
		{
			Advance();
			init.init->kind = INIT_PAREN;
			if (!AtSimple(OP_RPAREN) &&
			    !ParseInitializerClauseList(init.init->args))
				return false;
			if (!MatchSimple(OP_RPAREN))
				return false;
		}
		else if (AtSimple(OP_LBRACE))
		{
			AstExprPtr braced = ParseBracedInitList();
			if (!braced)
				return false;
			init.init->kind = INIT_BRACED;
			init.init->expr = move(braced);
		}
		else
			return false;
		decl.mem_initializers.push_back(move(init));
		decl.has_ctor_initializer = true;
		if (!MatchSimple(OP_COMMA))
			return true;
	}
}

// special-member-declaration / special-member-definition:
// member-function-specifier* special-member-name parameter-clause
// function-suffix* then `= default|delete ;`, `;`, or ctor-initializer?
// compound-statement.
AstDeclPtr AstParser::ParseSpecialMember(bool require_definition,
                                         bool qualified_default_only)
{
	State state = Save();
	AstDeclPtr decl = MakeDecl(require_definition
	                           ? DK_SPECIAL_MEMBER_DEFINITION
	                           : DK_SPECIAL_MEMBER_DECLARATION);
	for (;;)
	{
		SkipDeclAdornments();
		if (Peek().kind == PTOK_SIMPLE &&
		    IsMemberFunctionSpecifier(Peek().simple_type))
		{
			AstMemberSpecifier spec;
			spec.keyword = Peek().simple_type;
			spec.spelling = Peek().spelling;
			Advance();
			decl->member_specifiers.push_back(move(spec));
			continue;
		}
		break;
	}
	AstName name;
	if (!ParseSpecialMemberName(name))
	{
		Restore(state);
		return AstDeclPtr();
	}
	bool qualified = name.parts.size() > 1;
	decl->declarator = AstDeclaratorPtr(new AstDeclarator());
	AstDeclaratorItem id_item;
	id_item.kind = DI_ID;
	id_item.name = move(name);
	decl->declarator->items.push_back(move(id_item));
	AstParameterClausePtr clause;
	if (!ParseParameterClause(clause))
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclaratorItem params_item;
	params_item.kind = DI_PARAMS;
	params_item.params = move(clause);
	decl->declarator->items.push_back(move(params_item));
	ParseFunctionQualifiers(*decl->declarator);
	if (require_definition)
	{
		PushTransientScope();
		RegisterParameters(*decl->declarator);
		bool ok = true;
		if (AtSimple(OP_COLON))
			ok = ParseCtorInitializer(*decl);
		if (ok)
		{
			decl->body = ParseCompoundStatement();
			ok = decl->body != 0;
		}
		PopScope();
		if (!ok)
		{
			Restore(state);
			return AstDeclPtr();
		}
		return decl;
	}
	if (AtSimple(OP_ASS))
	{
		if (!ParseInitializer(decl->special_init, false) ||
		    (decl->special_init->kind != INIT_DEFAULT &&
		     decl->special_init->kind != INIT_DELETE))
		{
			Restore(state);
			return AstDeclPtr();
		}
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	// PA16 namespace-scope form: only a qualified `= default` special
	// member declaration is accepted outside a class body.
	if (qualified_default_only &&
	    (!qualified || !decl->special_init ||
	     decl->special_init->kind != INIT_DEFAULT))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// bit-field-declaration: decl-specifier-seq bit-field-declarator
// (, bit-field-declarator)* ; with declarator? : width each.
AstDeclPtr AstParser::ParseBitFieldDeclaration()
{
	State state = Save();
	AstDeclPtr decl = MakeDecl(DK_BIT_FIELD);
	if (!ParseSpecifierSeq(decl->specifiers, kDeclSpecifierSeq))
	{
		Restore(state);
		return AstDeclPtr();
	}
	for (;;)
	{
		AstDecl::BitField field;
		if (!AtSimple(OP_COLON))
		{
			if (!ParseDeclarator(field.declarator, true))
			{
				Restore(state);
				return AstDeclPtr();
			}
		}
		if (!MatchSimple(OP_COLON) ||
		    !(field.width = ParseAssignmentExpression()))
		{
			Restore(state);
			return AstDeclPtr();
		}
		if (field.declarator)
		{
			const AstName* id = field.declarator->IdName();
			if (id && id->IsPlainIdentifier())
				Register(id->parts[0].identifier, NF_VALUE);
		}
		decl->bit_fields.push_back(move(field));
		if (!MatchSimple(OP_COMMA))
			break;
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// class-member: dispatches to the forms and stamps the parsed node
// with its terminal token span, like ParseDeclaration.
AstDeclPtr AstParser::ParseMemberDeclaration()
{
	size_t begin = pos_;
	AstDeclPtr decl = ParseMemberDeclarationForms();
	if (decl)
	{
		decl->begin_token = begin;
		decl->end_token = pos_;
	}
	return decl;
}

// class-member forms: access labels, then the declaration forms in
// declaration-before-special-member order (a constructor only wins
// after the value reading fails).
AstDeclPtr AstParser::ParseMemberDeclarationForms()
{
	if (Peek().kind == PTOK_SIMPLE && IsAccessKeyword(Peek().simple_type) &&
	    AtSimple(OP_COLON, 1))
	{
		AstDeclPtr decl = MakeDecl(DK_ACCESS_LABEL);
		decl->access = Peek().simple_type;
		decl->access_spelling = Peek().spelling;
		Advance();
		Advance();
		return decl;
	}
	if (MatchSimple(OP_SEMICOLON))
		return MakeDecl(DK_EMPTY);
	if (AtSimple(KW_TEMPLATE))
		return ParseTemplateDeclaration();
	if (AtSimple(KW_USING))
	{
		AstDeclPtr alias = ParseAliasDeclaration();
		if (alias)
			return alias;
		return ParseUsingDeclarationOrDirective();
	}
	if (AtSimple(KW_STATIC_ASSERT))
		return ParseStaticAssertDeclaration();
	AstDeclPtr decl = ParseClassDeclaration();
	if (decl)
		return decl;
	decl = ParseEnumDeclaration();
	if (decl)
		return decl;
	decl = ParseFunctionDefinition();
	if (decl)
		return decl;
	decl = ParseSimpleDeclaration();
	if (decl)
		return decl;
	decl = ParseBitFieldDeclaration();
	if (decl)
		return decl;
	decl = ParseSpecialMember(true);
	if (decl)
		return decl;
	return ParseSpecialMember(false);
}

// type-parameter: (class|typename) ...? identifier? (= type-id)? and
// the template-template form, validated against the parameter follow
// set before committing.
bool AstParser::ParseTypeParameter(AstTemplateParameter& parameter)
{
	State state = Save();
	if (AtSimple(KW_TEMPLATE))
	{
		Advance();
		parameter.kind = TP_TEMPLATE;
		if (!MatchOpenAngle())
		{
			Restore(state);
			return false;
		}
		if (!AtCloseAngle() &&
		    !ParseTemplateParameterList(parameter.template_params))
		{
			Restore(state);
			return false;
		}
		if (!MatchCloseAngle() || !AtSimple(KW_CLASS))
		{
			Restore(state);
			return false;
		}
	}
	else if (!AtSimple(KW_CLASS) && !AtSimple(KW_TYPENAME))
		return false;
	parameter.key = Peek().simple_type;
	parameter.key_spelling = Peek().spelling;
	Advance();
	parameter.pack = MatchSimple(OP_DOTS);
	if (AtIdentifier())
	{
		parameter.name = Peek().spelling;
		Advance();
		Register(parameter.name, parameter.kind == TP_TEMPLATE
		         ? unsigned(NF_TYPE | NF_TEMPLATE) : unsigned(NF_TYPE));
	}
	if (AtSimple(OP_ASS))
	{
		Advance();
		if (!ParseTypeId(parameter.default_type))
		{
			Restore(state);
			return false;
		}
		parameter.has_default_type = true;
	}
	if (!AtSimple(OP_COMMA) && !AtCloseAngle())
	{
		Restore(state);
		return false;
	}
	return true;
}

// non-type-template-parameter: decl-specifier-seq ...? declarator?
// (= assignment-expression)?.
bool AstParser::ParseNonTypeParameter(AstTemplateParameter& parameter)
{
	State state = Save();
	parameter.kind = TP_NON_TYPE;
	if (!ParseSpecifierSeq(parameter.specifiers, kDeclSpecifierSeq))
	{
		Restore(state);
		return false;
	}
	parameter.pack = MatchSimple(OP_DOTS);
	State declarator_state = Save();
	AstDeclaratorPtr abstract;
	bool have = false;
	if (ParseDeclarator(abstract, false) && AtParameterFollow())
	{
		if (!abstract->Empty())
			parameter.declarator = move(abstract);
		have = true;
	}
	if (!have)
	{
		Restore(declarator_state);
		AstDeclaratorPtr named;
		if (ParseDeclarator(named, true) && AtParameterFollow())
		{
			const AstName* id = named->IdName();
			if (id && id->IsPlainIdentifier())
				Register(id->parts[0].identifier, NF_VALUE);
			parameter.declarator = move(named);
		}
		else
		{
			Restore(state);
			return false;
		}
	}
	if (AtSimple(OP_ASS))
	{
		Advance();
		parameter.default_expr = ParseAssignmentExpression();
		if (!parameter.default_expr)
		{
			Restore(state);
			return false;
		}
		parameter.has_default_expr = true;
		// Reference-format quirk: a declarator-less keyword-typed
		// parameter dumps a literal default as a token leaf.
		bool keywords_only = true;
		for (size_t i = 0; i < parameter.specifiers.size(); i++)
			if (parameter.specifiers[i].kind != SPEC_KEYWORD)
				keywords_only = false;
		parameter.default_token_form = !parameter.declarator &&
			keywords_only && parameter.default_expr->kind == EK_LITERAL;
	}
	if (!AtSimple(OP_COMMA) && !AtCloseAngle())
	{
		Restore(state);
		return false;
	}
	return true;
}

bool AstParser::ParseTemplateParameter(AstTemplateParameter& parameter)
{
	if (ParseTypeParameter(parameter))
		return true;
	return ParseNonTypeParameter(parameter);
}

bool AstParser::ParseTemplateParameterList(
	std::vector<AstTemplateParameter>& params)
{
	for (;;)
	{
		AstTemplateParameter parameter;
		if (!ParseTemplateParameter(parameter))
			return false;
		params.push_back(move(parameter));
		if (!MatchSimple(OP_COMMA))
			return true;
	}
}

// template-declaration: KW_TEMPLATE < template-parameter-list? >
// declaration, with the parameters scoped over the declaration.
AstDeclPtr AstParser::ParseTemplateDeclaration()
{
	State state = Save();
	if (!MatchSimple(KW_TEMPLATE) || !MatchOpenAngle())
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_TEMPLATE);
	PushScope(NewTable(), true);
	if (!AtCloseAngle())
	{
		if (!ParseTemplateParameterList(decl->template_params))
		{
			PopScope();
			Restore(state);
			return AstDeclPtr();
		}
	}
	if (!MatchCloseAngle())
	{
		PopScope();
		Restore(state);
		return AstDeclPtr();
	}
	decl->has_parameter_list = !decl->template_params.empty();
	size_t outer_depth = template_decl_scope_depth_;
	template_decl_scope_depth_ = scopes_.size();
	decl->inner = ParseDeclaration();
	if (!decl->inner)
		decl->inner = ParseSpecialMember(false);
	template_decl_scope_depth_ = outer_depth;
	PopScope();
	if (!decl->inner)
	{
		Restore(state);
		return AstDeclPtr();
	}
	RegisterTemplateName(*decl->inner);
	return decl;
}
