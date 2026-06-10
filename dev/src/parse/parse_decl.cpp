// Declarations: the declaration and block-declaration dispatchers,
// simple declarations, attributes, the shared specifier-sequence
// parser with the handout's decl-specifier-seq type-name rule, enums,
// namespaces, using/linkage forms, and template declarations.

#include "parser.h"

using std::move;

// declaration: block-declaration | function-definition |
//     template-declaration | explicit-instantiation |
//     explicit-specialization | linkage-specification |
//     namespace-definition | empty-declaration | attribute-declaration
ParseNodePtr Parser::ParseDeclaration()
{
	ParseNodePtr inner;
	if (AtSimple(OP_SEMICOLON))
	{
		inner = MakeParseNode("empty-declaration");
		inner->Add(MatchSimpleLeaf(OP_SEMICOLON));
	}
	else if (AtSimple(KW_NAMESPACE) ||
	         (AtSimple(KW_INLINE) && AtSimple(KW_NAMESPACE, 1)))
	{
		if (AtSimple(KW_NAMESPACE) && AtIdentifier(1) && AtSimple(OP_ASS, 2))
			inner = ParseNamespaceAliasDefinition();
		else
			inner = ParseNamespaceDefinition();
	}
	else if (AtSimple(KW_TEMPLATE))
	{
		if (AtSimple(OP_LT, 1))
		{
			inner = ParseTemplateDeclaration();
			if (!inner)
				inner = ParseExplicitSpecialization();
		}
		else
			inner = ParseExplicitInstantiation();
	}
	else if (AtSimple(KW_EXTERN) && AtSimple(KW_TEMPLATE, 1))
		inner = ParseExplicitInstantiation();
	else if (AtSimple(KW_EXTERN) && Peek(1).kind == PTOK_LITERAL)
		inner = ParseLinkageSpecification();
	else
	{
		inner = ParseBlockDeclaration();
		if (!inner)
			inner = ParseFunctionDefinition();
		if (!inner)
			inner = ParseAttributeDeclaration();
	}
	if (!inner)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("declaration");
	node->Add(move(inner));
	return node;
}

// block-declaration: simple-declaration | asm-definition |
//     namespace-alias-definition | using-declaration |
//     using-directive | static_assert-declaration | alias-declaration |
//     opaque-enum-declaration
ParseNodePtr Parser::ParseBlockDeclaration()
{
	ParseNodePtr inner;
	if (AtSimple(KW_ASM))
		inner = ParseAsmDefinition();
	else if (AtSimple(KW_NAMESPACE))
		inner = ParseNamespaceAliasDefinition();
	else if (AtSimple(KW_STATIC_ASSERT))
		inner = ParseStaticAssertDeclaration();
	else if (AtSimple(KW_USING))
	{
		if (AtSimple(KW_NAMESPACE, 1))
			inner = ParseUsingDirective();
		else
		{
			inner = ParseAliasDeclaration();
			if (!inner)
				inner = ParseUsingDeclaration();
		}
	}
	else
	{
		inner = ParseSimpleDeclaration();
		if (!inner)
			inner = ParseOpaqueEnumDeclaration();
		if (!inner)
			inner = ParseUsingDirective();  // attribute-prefixed form
	}
	if (!inner)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("block-declaration");
	node->Add(move(inner));
	return node;
}

// simple-declaration: attribute-specifier* decl-specifier-seq
//     init-declarator-list? OP_SEMICOLON
ParseNodePtr Parser::ParseSimpleDeclaration()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("simple-declaration");
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr specifiers = ParseDeclSpecifierSeq();
	if (!specifiers)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(specifiers));
	if (!AtSimple(OP_SEMICOLON))
	{
		ParseNodePtr declarators =
			ParseCommaList("init-declarator-list", &Parser::ParseInitDeclarator);
		if (!declarators)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(declarators));
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// static_assert-declaration: KW_STATIC_ASSERT OP_LPAREN
//     constant-expression OP_COMMA TT_LITERAL OP_RPAREN OP_SEMICOLON
ParseNodePtr Parser::ParseStaticAssertDeclaration()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_STATIC_ASSERT);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("static_assert-declaration");
	node->Add(move(kw));
	ParseNodePtr value;
	ParseNodePtr message;
	if (MatchSimple(OP_LPAREN) && (value = ParseConstantExpression()) &&
	    MatchSimple(OP_COMMA) && (message = MatchLiteralLeaf()) &&
	    MatchSimple(OP_RPAREN) && MatchSimple(OP_SEMICOLON))
	{
		node->Add(move(value));
		node->Add(move(message));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// attribute-declaration: attribute-specifier+ OP_SEMICOLON
ParseNodePtr Parser::ParseAttributeDeclaration()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("attribute-declaration");
	if (!ParseAttributeSpecifiers(node.get()) || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// alias-declaration: KW_USING TT_IDENTIFIER attribute-specifier*
//     OP_ASS type-id OP_SEMICOLON
ParseNodePtr Parser::ParseAliasDeclaration()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_USING);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr name = MatchIdentifierLeaf();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("alias-declaration");
	node->Add(move(kw));
	node->Add(move(name));
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr type;
	if (MatchSimple(OP_ASS) && (type = ParseTypeId()) &&
	    MatchSimple(OP_SEMICOLON))
	{
		node->Add(move(type));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// using-declaration: KW_USING KW_TYPENAME? nested-name-specifier
//     unqualified-id OP_SEMICOLON |
//     KW_USING OP_COLON2 unqualified-id OP_SEMICOLON
// (the second form is the nested-name root `::` followed by no
// suffixes, so one parse covers both)
ParseNodePtr Parser::ParseUsingDeclaration()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_USING);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("using-declaration");
	node->Add(move(kw));
	if (AtSimple(KW_TYPENAME))
		node->Add(MatchSimpleLeaf(KW_TYPENAME));
	ParseNodePtr nested = ParseNestedNameSpecifier();
	ParseNodePtr id;
	if (nested && (id = ParseUnqualifiedId()) && MatchSimple(OP_SEMICOLON))
	{
		node->Add(move(nested));
		node->Add(move(id));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// using-directive: attribute-specifier* KW_USING KW_NAMESPACE
//     nested-name-specifier? TT_IDENTIFIER OP_SEMICOLON
ParseNodePtr Parser::ParseUsingDirective()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("using-directive");
	ParseAttributeSpecifiers(node.get());
	if (!MatchSimple(KW_USING) || !MatchSimple(KW_NAMESPACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr name = MatchIdentifierLeaf();
	if (!name || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	return node;
}

// asm-definition: KW_ASM OP_LPAREN TT_LITERAL OP_RPAREN OP_SEMICOLON
ParseNodePtr Parser::ParseAsmDefinition()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_ASM);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("asm-definition");
	node->Add(move(kw));
	ParseNodePtr text;
	if (MatchSimple(OP_LPAREN) && (text = MatchLiteralLeaf()) &&
	    MatchSimple(OP_RPAREN) && MatchSimple(OP_SEMICOLON))
	{
		node->Add(move(text));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// namespace-definition: KW_INLINE? KW_NAMESPACE TT_IDENTIFIER?
//     OP_LBRACE namespace-body OP_RBRACE
ParseNodePtr Parser::ParseNamespaceDefinition()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("namespace-definition");
	if (AtSimple(KW_INLINE))
		node->Add(MatchSimpleLeaf(KW_INLINE));
	if (!MatchSimple(KW_NAMESPACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	if (AtIdentifier())
		node->Add(MatchIdentifierLeaf());
	if (!MatchSimple(OP_LBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr body = MakeParseNode("namespace-body");
	for (;;)
	{
		ParseNodePtr declaration = ParseDeclaration();
		if (!declaration)
			break;
		body->Add(move(declaration));
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(body));
	return node;
}

// namespace-alias-definition: KW_NAMESPACE TT_IDENTIFIER OP_ASS
//     qualified-namespace-specifier OP_SEMICOLON
ParseNodePtr Parser::ParseNamespaceAliasDefinition()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_NAMESPACE);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("namespace-alias-definition");
	node->Add(move(kw));
	ParseNodePtr name = MatchIdentifierLeaf();
	ParseNodePtr target;
	if (name && MatchSimple(OP_ASS) &&
	    (target = ParseQualifiedNamespaceSpecifier()) &&
	    MatchSimple(OP_SEMICOLON))
	{
		node->Add(move(name));
		node->Add(move(target));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// qualified-namespace-specifier: nested-name-specifier? namespace-name
ParseNodePtr Parser::ParseQualifiedNamespaceSpecifier()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("qualified-namespace-specifier");
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr name = ParseNamespaceName();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	return node;
}

// linkage-specification: KW_EXTERN TT_LITERAL OP_LBRACE declaration*
//     OP_RBRACE | KW_EXTERN TT_LITERAL declaration
ParseNodePtr Parser::ParseLinkageSpecification()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_EXTERN);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr language = MatchLiteralLeaf();
	if (!language)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("linkage-specification");
	node->Add(move(kw));
	node->Add(move(language));
	if (MatchSimple(OP_LBRACE))
	{
		for (;;)
		{
			ParseNodePtr declaration = ParseDeclaration();
			if (!declaration)
				break;
			node->Add(move(declaration));
		}
		if (!MatchSimple(OP_RBRACE))
		{
			Restore(state);
			return ParseNodePtr();
		}
		return node;
	}
	ParseNodePtr declaration = ParseDeclaration();
	if (!declaration)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(declaration));
	return node;
}

// template-declaration: KW_TEMPLATE OP_LT template-parameter-list
//     close-angle-bracket declaration
ParseNodePtr Parser::ParseTemplateDeclaration()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_TEMPLATE);
	if (!kw || !MatchOpenAngle())
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("template-declaration");
	node->Add(move(kw));
	ParseNodePtr parameters = ParseCommaList("template-parameter-list",
	                                         &Parser::ParseTemplateParameter);
	ParseNodePtr close;
	ParseNodePtr declaration;
	if (parameters && (close = ParseCloseAngleBracket()) &&
	    (declaration = ParseDeclaration()))
	{
		node->Add(move(parameters));
		node->Add(move(declaration));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// template-parameter: type-parameter | parameter-declaration
// (the type-parameter reading wins, validated at a list boundary)
ParseNodePtr Parser::ParseTemplateParameter()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("template-parameter");
	ParseNodePtr parameter = ParseTypeParameter();
	if (parameter && AtTemplateArgumentFollow())
	{
		node->Add(move(parameter));
		return node;
	}
	Restore(state);
	parameter = ParseParameterDeclaration();
	if (!parameter)
		return ParseNodePtr();
	node->Add(move(parameter));
	return node;
}

// type-parameter: (KW_CLASS | KW_TYPENAME) OP_DOTS? TT_IDENTIFIER? |
//     (KW_CLASS | KW_TYPENAME) TT_IDENTIFIER? OP_ASS type-id |
//     KW_TEMPLATE OP_LT template-parameter-list close-angle-bracket
//     KW_CLASS (same identifier/default tails, with an id-expression
//     default)
ParseNodePtr Parser::ParseTypeParameter()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("type-parameter");
	bool template_form = false;
	if (AtSimple(KW_TEMPLATE))
	{
		node->Add(MatchSimpleLeaf(KW_TEMPLATE));
		ParseNodePtr parameters;
		ParseNodePtr close;
		if (!MatchOpenAngle() ||
		    !(parameters = ParseCommaList("template-parameter-list",
		                                  &Parser::ParseTemplateParameter)) ||
		    !(close = ParseCloseAngleBracket()))
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(parameters));
		template_form = true;
		if (!AtSimple(KW_CLASS))
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(MatchSimpleLeaf(KW_CLASS));
	}
	else if (AtSimple(KW_CLASS) || AtSimple(KW_TYPENAME))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
	}
	else
		return ParseNodePtr();
	if (AtSimple(OP_DOTS))
	{
		node->Add(MatchSimpleLeaf(OP_DOTS));
		if (AtIdentifier())
			node->Add(MatchIdentifierLeaf());
		return node;
	}
	if (AtIdentifier())
		node->Add(MatchIdentifierLeaf());
	if (AtSimple(OP_ASS))
	{
		State default_state = Save();
		MatchSimple(OP_ASS);
		ParseNodePtr value;
		if (template_form)
			value = ParseIdExpression();
		else
			value = ParseTypeId();
		if (!value)
		{
			Restore(default_state);
			return node;
		}
		node->Add(move(value));
	}
	return node;
}

// explicit-instantiation: KW_EXTERN? KW_TEMPLATE declaration
ParseNodePtr Parser::ParseExplicitInstantiation()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("explicit-instantiation");
	if (AtSimple(KW_EXTERN))
		node->Add(MatchSimpleLeaf(KW_EXTERN));
	if (!MatchSimple(KW_TEMPLATE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr declaration = ParseDeclaration();
	if (!declaration)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(declaration));
	return node;
}

// explicit-specialization: KW_TEMPLATE OP_LT close-angle-bracket
//     declaration
ParseNodePtr Parser::ParseExplicitSpecialization()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_TEMPLATE);
	if (!kw || !MatchOpenAngle())
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("explicit-specialization");
	node->Add(move(kw));
	ParseNodePtr close = ParseCloseAngleBracket();
	ParseNodePtr declaration;
	if (close && (declaration = ParseDeclaration()))
	{
		node->Add(move(declaration));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// attribute-specifier* appended to parent; true when at least one was
// parsed.
bool Parser::ParseAttributeSpecifiers(ParseNode* parent)
{
	bool any = false;
	for (;;)
	{
		ParseNodePtr specifier = ParseAttributeSpecifier();
		if (!specifier)
			break;
		parent->Add(move(specifier));
		any = true;
	}
	return any;
}

// attribute-specifier: OP_LSQUARE OP_LSQUARE attribute-list
//     OP_RSQUARE OP_RSQUARE | alignment-specifier
ParseNodePtr Parser::ParseAttributeSpecifier()
{
	if (AtSimple(KW_ALIGNAS))
		return ParseAlignmentSpecifier();
	if (!AtSimple(OP_LSQUARE) || !AtSimple(OP_LSQUARE, 1))
		return ParseNodePtr();
	State state = Save();
	Advance();
	Advance();
	ParseNodePtr node = MakeParseNode("attribute-specifier");
	ParseNodePtr list = ParseAttributeList();
	if (!list || !MatchSimple(OP_RSQUARE) || !MatchSimple(OP_RSQUARE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(list));
	return node;
}

// alignment-specifier: KW_ALIGNAS OP_LPAREN (type-id |
//     assignment-expression) OP_DOTS? OP_RPAREN
// (the type reading wins, 8.2/3)
ParseNodePtr Parser::ParseAlignmentSpecifier()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_ALIGNAS);
	if (!kw || !MatchSimple(OP_LPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("alignment-specifier");
	node->Add(move(kw));
	State operand_state = Save();
	ParseNodePtr operand = ParseTypeId();
	if (!(operand && (AtSimple(OP_DOTS) || AtSimple(OP_RPAREN))))
	{
		Restore(operand_state);
		operand = ParseAssignmentExpression();
	}
	if (!operand)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(operand));
	if (AtSimple(OP_DOTS))
		node->Add(MatchSimpleLeaf(OP_DOTS));
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// attribute-list: attribute-part (OP_COMMA attribute-part)* where
// attribute-part: attribute? | attribute OP_DOTS (parts may be empty,
// so the list itself never fails; the comma drives termination)
ParseNodePtr Parser::ParseAttributeList()
{
	ParseNodePtr node = MakeParseNode("attribute-list");
	for (;;)
	{
		ParseNodePtr part = MakeParseNode("attribute-part");
		ParseNodePtr attribute = ParseAttribute();
		if (attribute)
		{
			part->Add(move(attribute));
			if (AtSimple(OP_DOTS))
				part->Add(MatchSimpleLeaf(OP_DOTS));
		}
		node->Add(move(part));
		if (!MatchSimple(OP_COMMA))
			break;
	}
	return node;
}

// attribute: attribute-token attribute-argument-clause? with
// attribute-token: TT_IDENTIFIER | attribute-namespace OP_COLON2
//     TT_IDENTIFIER
ParseNodePtr Parser::ParseAttribute()
{
	if (!AtIdentifier())
		return ParseNodePtr();
	State state = Save();
	ParseNodePtr node = MakeParseNode("attribute");
	node->Add(MatchIdentifierLeaf());
	if (MatchSimple(OP_COLON2))
	{
		ParseNodePtr name = MatchIdentifierLeaf();
		if (!name)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(name));
	}
	ParseNodePtr arguments = ParseAttributeArgumentClause();
	if (arguments)
		node->Add(move(arguments));
	return node;
}

// attribute-argument-clause: OP_LPAREN balanced-token* OP_RPAREN
ParseNodePtr Parser::ParseAttributeArgumentClause()
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("attribute-argument-clause");
	for (;;)
	{
		ParseNodePtr token = ParseBalancedToken();
		if (!token)
			break;
		node->Add(move(token));
	}
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// balanced-token: bracketed balanced-token* groups or ST_NONPAREN (any
// other token except ST_EOF)
ParseNodePtr Parser::ParseBalancedToken()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_EOF)
		return ParseNodePtr();
	if (token.kind == PTOK_SIMPLE)
	{
		ETokenType open = token.simple_type;
		if (open == OP_RPAREN || open == OP_RSQUARE || open == OP_RBRACE)
			return ParseNodePtr();
		if (open == OP_LPAREN || open == OP_LSQUARE || open == OP_LBRACE)
		{
			ETokenType close = open == OP_LPAREN
				? OP_RPAREN
				: (open == OP_LSQUARE ? OP_RSQUARE : OP_RBRACE);
			State state = Save();
			Advance();
			ParseNodePtr node = MakeParseNode("balanced-token");
			for (;;)
			{
				ParseNodePtr inner = ParseBalancedToken();
				if (!inner)
					break;
				node->Add(move(inner));
			}
			if (!MatchSimple(close))
			{
				Restore(state);
				return ParseNodePtr();
			}
			return node;
		}
	}
	ParseNodePtr node = MakeParseNode("balanced-token");
	node->Add(MakeTokenLeaf(token));
	Advance();
	return node;
}

ParseNodePtr Parser::ParseDeclSpecifierSeq()
{
	return ParseSpecifierSeq(kDeclSpecifierSeq, "decl-specifier-seq");
}

ParseNodePtr Parser::ParseTypeSpecifierSeq()
{
	return ParseSpecifierSeq(kTypeSpecifierSeq, "type-specifier-seq");
}

ParseNodePtr Parser::ParseTrailingTypeSpecifierSeq()
{
	return ParseSpecifierSeq(kTrailingTypeSpecifierSeq,
	                         "trailing-type-specifier-seq");
}

// The shared specifier+ attribute-specifier* sequence parser.
// Implements the handout's decl-specifier-seq rule (applied uniformly
// to the type-specifier sequences): a type-name is part of the
// sequence iff no previous type-specifier other than a cv-qualifier
// has been seen, so in `C1 C2;` the second class-name starts the
// declarator instead.
ParseNodePtr Parser::ParseSpecifierSeq(ESpecifierSeqKind kind,
                                       const char* name)
{
	ParseNodePtr node = MakeParseNode(name);
	bool seen_type = false;
	int count = 0;
	for (;;)
	{
		ParseNodePtr specifier = ParseOneSpecifier(kind, &seen_type);
		if (!specifier)
			break;
		node->Add(move(specifier));
		count++;
	}
	if (count == 0)
		return ParseNodePtr();
	ParseAttributeSpecifiers(node.get());
	return node;
}

ParseNodePtr Parser::ParseOneSpecifier(ESpecifierSeqKind kind,
                                       bool* seen_type)
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_IDENTIFIER)
	{
		// identifier-led simple-type-specifier (the type-name forms):
		// gated by the decl-specifier-seq rule
		if (*seen_type)
			return ParseNodePtr();
		ParseNodePtr specifier = ParseTypeNameSpecifier();
		if (specifier)
			*seen_type = true;
		return specifier;
	}
	if (token.kind != PTOK_SIMPLE)
		return ParseNodePtr();
	switch (token.simple_type)
	{
	case KW_REGISTER:
	case KW_STATIC:
	case KW_THREAD_LOCAL:
	case KW_EXTERN:
	case KW_MUTABLE:
	case KW_INLINE:
	case KW_VIRTUAL:
	case KW_EXPLICIT:
	case KW_FRIEND:
	case KW_TYPEDEF:
	case KW_CONSTEXPR:
	{
		// storage-class-specifiers, function-specifiers, friend,
		// typedef, constexpr: decl-specifier-seq only
		if (kind != kDeclSpecifierSeq)
			return ParseNodePtr();
		ParseNodePtr leaf = MakeTokenLeaf(token);
		Advance();
		return leaf;
	}
	case KW_CONST:
	case KW_VOLATILE:
	{
		// cv-qualifiers never count as the type
		ParseNodePtr leaf = MakeTokenLeaf(token);
		Advance();
		return leaf;
	}
	case KW_CHAR:
	case KW_CHAR16_T:
	case KW_CHAR32_T:
	case KW_WCHAR_T:
	case KW_BOOL:
	case KW_SHORT:
	case KW_INT:
	case KW_LONG:
	case KW_SIGNED:
	case KW_UNSIGNED:
	case KW_FLOAT:
	case KW_DOUBLE:
	case KW_VOID:
	case KW_AUTO:
	{
		// keyword simple-type-specifiers are never gated (so
		// `unsigned int` and `long long` parse), but they set the seen
		// flag
		*seen_type = true;
		ParseNodePtr leaf = MakeTokenLeaf(token);
		Advance();
		return leaf;
	}
	case KW_CLASS:
	case KW_STRUCT:
	case KW_UNION:
	case KW_ENUM:
	case KW_TYPENAME:
	case KW_DECLTYPE:
	case OP_COLON2:
		return ParseTaggedTypeSpecifier(kind, seen_type);
	default:
		return ParseNodePtr();
	}
}

// The class-key/enum/typename/decltype/::-led type-specifiers of a
// specifier sequence: the defining specifiers fall back to their
// elaborated forms, and the nested-name forms that end in a type-name
// fall under the sequence's type-name gate.
ParseNodePtr Parser::ParseTaggedTypeSpecifier(ESpecifierSeqKind kind,
                                              bool* seen_type)
{
	const ParseToken& token = Peek();
	if (token.simple_type == KW_CLASS || token.simple_type == KW_STRUCT ||
	    token.simple_type == KW_UNION || token.simple_type == KW_ENUM)
	{
		if (kind != kTrailingTypeSpecifierSeq)
		{
			ParseNodePtr specifier = token.simple_type == KW_ENUM
				? ParseEnumSpecifier()
				: ParseClassSpecifier();
			if (specifier)
			{
				*seen_type = true;
				return specifier;
			}
		}
		ParseNodePtr specifier = ParseElaboratedTypeSpecifier();
		if (specifier)
			*seen_type = true;
		return specifier;
	}
	if (token.simple_type == KW_TYPENAME)
	{
		ParseNodePtr specifier = ParseTypenameSpecifier();
		if (specifier)
			*seen_type = true;
		return specifier;
	}
	// a bare decltype-specifier is keyword-led and ungated; the
	// `decltype(...)::` and `::`-rooted forms end in a type-name and so
	// fall under the gate
	if (token.simple_type == KW_DECLTYPE)
	{
		State state = Save();
		ParseNodePtr decltype_spec = ParseDecltypeSpecifier();
		if (!decltype_spec)
			return ParseNodePtr();
		if (!AtSimple(OP_COLON2))
		{
			*seen_type = true;
			return decltype_spec;
		}
		// it roots a nested-name-specifier after all
		Restore(state);
	}
	if (*seen_type)
		return ParseNodePtr();
	ParseNodePtr specifier = ParseTypeNameSpecifier();
	if (specifier)
		*seen_type = true;
	return specifier;
}

// The identifier/::/decltype-led simple-type-specifier forms:
// nested-name-specifier? type-name |
// nested-name-specifier KW_TEMPLATE simple-template-id
ParseNodePtr Parser::ParseTypeNameSpecifier()
{
	State state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
	{
		ParseNodePtr node = MakeParseNode("simple-type-specifier");
		node->Add(move(nested));
		if (AtSimple(KW_TEMPLATE))
		{
			ParseNodePtr template_kw = MatchSimpleLeaf(KW_TEMPLATE);
			ParseNodePtr id = ParseSimpleTemplateId();
			if (id)
			{
				node->Add(move(template_kw));
				node->Add(move(id));
				return node;
			}
		}
		else
		{
			ParseNodePtr name = ParseTypeName();
			if (name)
			{
				node->Add(move(name));
				return node;
			}
		}
		// the greedy nested-name root consumed the type-name itself
		// (`Cfoo::~Cfoo() {}`): retry as an unqualified type-name and
		// leave the qualification to the declarator
		Restore(state);
	}
	ParseNodePtr name = ParseTypeName();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("simple-type-specifier");
	node->Add(move(name));
	return node;
}

// simple-type-specifier (standalone, for postfix-root functional
// casts): the keyword forms, decltype, and the type-name forms.
ParseNodePtr Parser::ParseSimpleTypeSpecifier()
{
	static const ETokenType kTypeKeywords[] = {
		KW_CHAR, KW_CHAR16_T, KW_CHAR32_T, KW_WCHAR_T, KW_BOOL, KW_SHORT,
		KW_INT, KW_LONG, KW_SIGNED, KW_UNSIGNED, KW_FLOAT, KW_DOUBLE,
		KW_VOID, KW_AUTO
	};
	for (size_t i = 0; i < sizeof(kTypeKeywords) / sizeof(ETokenType); i++)
	{
		if (AtSimple(kTypeKeywords[i]))
		{
			ParseNodePtr node = MakeParseNode("simple-type-specifier");
			node->Add(MatchSimpleLeaf(kTypeKeywords[i]));
			return node;
		}
	}
	if (AtSimple(KW_DECLTYPE) && !AtSimple(OP_COLON2, 1))
	{
		State state = Save();
		ParseNodePtr decltype_spec = ParseDecltypeSpecifier();
		if (decltype_spec && !AtSimple(OP_COLON2))
		{
			ParseNodePtr node = MakeParseNode("simple-type-specifier");
			node->Add(move(decltype_spec));
			return node;
		}
		Restore(state);
	}
	return ParseTypeNameSpecifier();
}

// elaborated-type-specifier:
//     class-key nested-name-specifier? KW_TEMPLATE? simple-template-id
//     class-key attribute-specifier* nested-name-specifier?
//         TT_IDENTIFIER
//     KW_ENUM nested-name-specifier? TT_IDENTIFIER
ParseNodePtr Parser::ParseElaboratedTypeSpecifier()
{
	State state = Save();
	if (AtSimple(KW_ENUM))
	{
		ParseNodePtr node = MakeParseNode("elaborated-type-specifier");
		node->Add(MatchSimpleLeaf(KW_ENUM));
		ParseNodePtr nested = ParseNestedNameSpecifier();
		if (nested)
			node->Add(move(nested));
		ParseNodePtr name = MatchIdentifierLeaf();
		if (!name)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(name));
		return node;
	}
	ParseNodePtr key = ParseClassKey();
	if (!key)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("elaborated-type-specifier");
	node->Add(move(key));
	// template-id form first (longest; takes no attributes)
	State form_state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	ParseNodePtr template_kw;
	if (AtSimple(KW_TEMPLATE))
		template_kw = MatchSimpleLeaf(KW_TEMPLATE);
	ParseNodePtr id = ParseSimpleTemplateId();
	if (id)
	{
		if (nested)
			node->Add(move(nested));
		if (template_kw)
			node->Add(move(template_kw));
		node->Add(move(id));
		return node;
	}
	Restore(form_state);
	nested.reset();
	ParseAttributeSpecifiers(node.get());
	nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr name = MatchIdentifierLeaf();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	return node;
}

// enum-specifier: enum-head OP_LBRACE enumerator-list? OP_COMMA?
//     OP_RBRACE
ParseNodePtr Parser::ParseEnumSpecifier()
{
	State state = Save();
	ParseNodePtr head = ParseEnumHead();
	if (!head)
		return ParseNodePtr();
	if (!MatchSimple(OP_LBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("enum-specifier");
	node->Add(move(head));
	if (!AtSimple(OP_RBRACE))
	{
		ParseNodePtr enumerators =
			ParseCommaList("enumerator-list",
			               &Parser::ParseEnumeratorDefinition);
		if (!enumerators)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(enumerators));
		MatchSimple(OP_COMMA);
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// enum-head: enum-key attribute-specifier* TT_IDENTIFIER? enum-base? |
//     enum-key attribute-specifier* nested-name-specifier
//     TT_IDENTIFIER enum-base?
ParseNodePtr Parser::ParseEnumHead()
{
	ParseNodePtr key = ParseEnumKey();
	if (!key)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("enum-head");
	node->Add(move(key));
	ParseAttributeSpecifiers(node.get());
	State name_state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested && AtIdentifier())
	{
		node->Add(move(nested));
		node->Add(MatchIdentifierLeaf());
	}
	else
	{
		Restore(name_state);
		if (AtIdentifier())
			node->Add(MatchIdentifierLeaf());
	}
	ParseNodePtr base = ParseEnumBase();
	if (base)
		node->Add(move(base));
	return node;
}

// enum-key: KW_ENUM | KW_ENUM KW_CLASS | KW_ENUM KW_STRUCT
ParseNodePtr Parser::ParseEnumKey()
{
	ParseNodePtr kw = MatchSimpleLeaf(KW_ENUM);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("enum-key");
	node->Add(move(kw));
	if (AtSimple(KW_CLASS) || AtSimple(KW_STRUCT))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
	}
	return node;
}

// enum-base: OP_COLON type-specifier-seq
ParseNodePtr Parser::ParseEnumBase()
{
	State state = Save();
	if (!MatchSimple(OP_COLON))
		return ParseNodePtr();
	ParseNodePtr specifiers = ParseTypeSpecifierSeq();
	if (!specifiers)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("enum-base");
	node->Add(move(specifiers));
	return node;
}

// enumerator-definition: enumerator (OP_ASS constant-expression)?
ParseNodePtr Parser::ParseEnumeratorDefinition()
{
	if (!AtIdentifier())
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("enumerator-definition");
	node->Add(MatchIdentifierLeaf());
	if (AtSimple(OP_ASS))
	{
		State state = Save();
		MatchSimple(OP_ASS);
		ParseNodePtr value = ParseConstantExpression();
		if (value)
			node->Add(move(value));
		else
			Restore(state);
	}
	return node;
}

// opaque-enum-declaration: enum-key attribute-specifier*
//     TT_IDENTIFIER enum-base? OP_SEMICOLON
ParseNodePtr Parser::ParseOpaqueEnumDeclaration()
{
	State state = Save();
	ParseNodePtr key = ParseEnumKey();
	if (!key)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("opaque-enum-declaration");
	node->Add(move(key));
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr name = MatchIdentifierLeaf();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	ParseNodePtr base = ParseEnumBase();
	if (base)
		node->Add(move(base));
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}
