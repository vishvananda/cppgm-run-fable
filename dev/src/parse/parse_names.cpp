// Names and templates: the *-name nonterminals with their mock name
// lookup gates, nested-name-specifiers, id-expressions, the three
// operator-id forms, and template-ids/arguments (with the 14.2.3
// close-angle-bracket discipline at every template-argument list).

#include "parser.h"

using std::move;

// type-name: class-name | enum-name | typedef-name | simple-template-id
// The simple-template-id alternative is tried first (longest match);
// the identifier alternatives are the mock-gated C/E/Y categories.
ParseNodePtr Parser::ParseTypeName()
{
	ParseNodePtr node = MakeParseNode("type-name");
	ParseNodePtr id = ParseSimpleTemplateId();
	if (id)
	{
		node->Add(move(id));
		return node;
	}
	if (AtIdentifier() &&
	    Peek().HasFlag(PTF_CLASS_NAME | PTF_ENUM_NAME | PTF_TYPEDEF_NAME))
	{
		node->Add(MatchIdentifierLeaf());
		return node;
	}
	return ParseNodePtr();
}

// class-name: TT_IDENTIFIER | simple-template-id
ParseNodePtr Parser::ParseClassName()
{
	ParseNodePtr node = MakeParseNode("class-name");
	ParseNodePtr id = ParseSimpleTemplateId();
	if (id)
	{
		node->Add(move(id));
		return node;
	}
	if (AtIdentifier() && Peek().HasFlag(PTF_CLASS_NAME))
	{
		node->Add(MatchIdentifierLeaf());
		return node;
	}
	return ParseNodePtr();
}

ParseNodePtr Parser::ParseNamespaceName()
{
	if (!AtIdentifier() || !Peek().HasFlag(PTF_NAMESPACE_NAME))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("namespace-name");
	node->Add(MatchIdentifierLeaf());
	return node;
}

// simple-template-id:
//     template-name OP_LT template-argument-list? close-angle-bracket
ParseNodePtr Parser::ParseSimpleTemplateId()
{
	return MemoParse(kMemoSimpleTemplateId,
	                 &Parser::ParseSimpleTemplateIdRule);
}

ParseNodePtr Parser::ParseSimpleTemplateIdRule()
{
	State state = Save();
	if (!AtIdentifier() || !Peek().HasFlag(PTF_TEMPLATE_NAME))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("simple-template-id");
	node->Add(MatchIdentifierLeaf());
	if (!MatchOpenAngle())
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr arguments = ParseTemplateArgumentList();
	ParseNodePtr close = ParseCloseAngleBracket();
	if (!close)
	{
		Restore(state);
		return ParseNodePtr();
	}
	if (arguments)
		node->Add(move(arguments));
	node->Add(move(close));
	return node;
}

// id-expression: unqualified-id | qualified-id (qualified first: it
// consumes the longer nested-name-specifier prefix)
ParseNodePtr Parser::ParseIdExpression()
{
	return MemoParse(kMemoIdExpression, &Parser::ParseIdExpressionRule);
}

ParseNodePtr Parser::ParseIdExpressionRule()
{
	ParseNodePtr id = ParseQualifiedId();
	if (!id)
		id = ParseUnqualifiedId();
	if (!id)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("id-expression");
	node->Add(move(id));
	return node;
}

// unqualified-id: TT_IDENTIFIER | operator-function-id |
//     conversion-function-id | literal-operator-id |
//     OP_COMPL class-name | OP_COMPL decltype-specifier | template-id
ParseNodePtr Parser::ParseUnqualifiedId()
{
	if (AtSimple(OP_COMPL))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("unqualified-id");
		node->Add(MatchSimpleLeaf(OP_COMPL));
		ParseNodePtr name;
		if (AtSimple(KW_DECLTYPE))
			name = ParseDecltypeSpecifier();
		else
			name = ParseClassName();
		if (!name)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(name));
		return node;
	}
	if (AtSimple(KW_OPERATOR))
		return ParseOperatorIdForm();
	// template-id (here always a simple-template-id) before the plain
	// identifier so `T1<...>` is not left dangling after `T1`.
	ParseNodePtr id = ParseSimpleTemplateId();
	if (!id && AtIdentifier())
		id = MatchIdentifierLeaf();
	if (!id)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("unqualified-id");
	node->Add(move(id));
	return node;
}

// qualified-id: nested-name-specifier KW_TEMPLATE? unqualified-id
ParseNodePtr Parser::ParseQualifiedId()
{
	State state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (!nested)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("qualified-id");
	node->Add(move(nested));
	if (AtSimple(KW_TEMPLATE))
		node->Add(MatchSimpleLeaf(KW_TEMPLATE));
	ParseNodePtr id = ParseUnqualifiedId();
	if (!id)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(id));
	return node;
}

// nested-name-specifier:
//     nested-name-specifier-root nested-name-specifier-suffix*
ParseNodePtr Parser::ParseNestedNameSpecifier()
{
	return MemoParse(kMemoNestedNameSpecifier,
	                 &Parser::ParseNestedNameSpecifierRule);
}

ParseNodePtr Parser::ParseNestedNameSpecifierRule()
{
	ParseNodePtr root = ParseNestedNameSpecifierRoot();
	if (!root)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("nested-name-specifier");
	node->Add(move(root));
	for (;;)
	{
		ParseNodePtr suffix = ParseNestedNameSpecifierSuffix();
		if (!suffix)
			break;
		node->Add(move(suffix));
	}
	return node;
}

// nested-name-specifier-root: OP_COLON2 | type-name OP_COLON2 |
//     namespace-name OP_COLON2 | decltype-specifier OP_COLON2
ParseNodePtr Parser::ParseNestedNameSpecifierRoot()
{
	ParseNodePtr node = MakeParseNode("nested-name-specifier-root");
	if (AtSimple(OP_COLON2))
	{
		node->Add(MatchSimpleLeaf(OP_COLON2));
		return node;
	}
	State state = Save();
	if (AtSimple(KW_DECLTYPE))
	{
		ParseNodePtr decltype_spec = ParseDecltypeSpecifier();
		if (decltype_spec && AtSimple(OP_COLON2))
		{
			node->Add(move(decltype_spec));
			node->Add(MatchSimpleLeaf(OP_COLON2));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr name = ParseTypeName();
	if (name && AtSimple(OP_COLON2))
	{
		node->Add(move(name));
		node->Add(MatchSimpleLeaf(OP_COLON2));
		return node;
	}
	Restore(state);
	name = ParseNamespaceName();
	if (name && AtSimple(OP_COLON2))
	{
		node->Add(move(name));
		node->Add(MatchSimpleLeaf(OP_COLON2));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// nested-name-specifier-suffix: TT_IDENTIFIER OP_COLON2 |
//     KW_TEMPLATE? simple-template-id OP_COLON2
ParseNodePtr Parser::ParseNestedNameSpecifierSuffix()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("nested-name-specifier-suffix");
	if (AtIdentifier() && AtSimple(OP_COLON2, 1))
	{
		node->Add(MatchIdentifierLeaf());
		node->Add(MatchSimpleLeaf(OP_COLON2));
		return node;
	}
	if (AtSimple(KW_TEMPLATE))
		node->Add(MatchSimpleLeaf(KW_TEMPLATE));
	ParseNodePtr id = ParseSimpleTemplateId();
	if (id && AtSimple(OP_COLON2))
	{
		node->Add(move(id));
		node->Add(MatchSimpleLeaf(OP_COLON2));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// decltype-specifier: KW_DECLTYPE OP_LPAREN expression OP_RPAREN
ParseNodePtr Parser::ParseDecltypeSpecifier()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_DECLTYPE);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr expression;
	if (!MatchSimple(OP_LPAREN) || !(expression = ParseExpression()) ||
	    !MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("decltype-specifier");
	node->Add(move(kw));
	node->Add(move(expression));
	return node;
}

// typename-specifier: KW_TYPENAME nested-name-specifier TT_IDENTIFIER |
//     KW_TYPENAME nested-name-specifier KW_TEMPLATE? simple-template-id
ParseNodePtr Parser::ParseTypenameSpecifier()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_TYPENAME);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (!nested)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("typename-specifier");
	node->Add(move(kw));
	node->Add(move(nested));
	State name_state = Save();
	ParseNodePtr template_kw = MatchSimpleLeaf(KW_TEMPLATE);
	ParseNodePtr id = ParseSimpleTemplateId();
	if (id)
	{
		if (template_kw)
			node->Add(move(template_kw));
		node->Add(move(id));
		return node;
	}
	Restore(name_state);
	if (AtIdentifier())
	{
		node->Add(MatchIdentifierLeaf());
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// Dispatches the unqualified-id forms headed by KW_OPERATOR, extending
// operator-function-id / literal-operator-id with a template-argument
// list (template-id) when one follows.
ParseNodePtr Parser::ParseOperatorIdForm()
{
	ParseNodePtr id = ParseLiteralOperatorId();
	if (!id)
		id = ParseOperatorFunctionId();
	if (id)
	{
		State state = Save();
		if (MatchOpenAngle())
		{
			ParseNodePtr arguments = ParseTemplateArgumentList();
			ParseNodePtr close = ParseCloseAngleBracket();
			if (close)
			{
				ParseNodePtr node = MakeParseNode("template-id");
				node->Add(move(id));
				if (arguments)
					node->Add(move(arguments));
				node->Add(move(close));
				return node;
			}
			Restore(state);
		}
		return id;
	}
	return ParseConversionFunctionId();
}

// operator-function-id: KW_OPERATOR followed by one overloadable
// operator (with `new []`/`delete []`, `()`, `[]`, and the split
// `>>` handled as multi-token spellings).
ParseNodePtr Parser::ParseOperatorFunctionId()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_OPERATOR);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("operator-function-id");
	node->Add(move(kw));
	if (AtSimple(KW_NEW) || AtSimple(KW_DELETE))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		State array_state = Save();
		ParseNodePtr open = MatchSimpleLeaf(OP_LSQUARE);
		if (open)
		{
			ParseNodePtr close = MatchSimpleLeaf(OP_RSQUARE);
			if (close)
			{
				node->Add(move(open));
				node->Add(move(close));
			}
			else
				Restore(array_state);
		}
		return node;
	}
	if ((AtSimple(OP_LPAREN) && AtSimple(OP_RPAREN, 1)) ||
	    (AtSimple(OP_LSQUARE) && AtSimple(OP_RSQUARE, 1)))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		return node;
	}
	if (Peek().kind == PTOK_RSHIFT_1 && Peek(1).kind == PTOK_RSHIFT_2)
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		return node;
	}
	static const ETokenType kOverloadableOps[] = {
		OP_PLUS, OP_MINUS, OP_STAR, OP_DIV, OP_MOD, OP_XOR, OP_AMP,
		OP_BOR, OP_COMPL, OP_LNOT, OP_ASS, OP_LT, OP_GT, OP_PLUSASS,
		OP_MINUSASS, OP_STARASS, OP_DIVASS, OP_MODASS, OP_XORASS,
		OP_BANDASS, OP_BORASS, OP_LSHIFT, OP_RSHIFTASS, OP_LSHIFTASS,
		OP_EQ, OP_NE, OP_LE, OP_GE, OP_LAND, OP_LOR, OP_INC, OP_DEC,
		OP_COMMA, OP_ARROWSTAR, OP_ARROW
	};
	for (size_t i = 0; i < sizeof(kOverloadableOps) / sizeof(ETokenType); i++)
	{
		if (AtSimple(kOverloadableOps[i]))
		{
			node->Add(MatchSimpleLeaf(kOverloadableOps[i]));
			return node;
		}
	}
	Restore(state);
	return ParseNodePtr();
}

// conversion-function-id: KW_OPERATOR conversion-type-id
ParseNodePtr Parser::ParseConversionFunctionId()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_OPERATOR);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr type = ParseConversionTypeId();
	if (!type)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("conversion-function-id");
	node->Add(move(kw));
	node->Add(move(type));
	return node;
}

// conversion-type-id: type-specifier-seq ptr-operator*
ParseNodePtr Parser::ParseConversionTypeId()
{
	ParseNodePtr specifiers = ParseTypeSpecifierSeq();
	if (!specifiers)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("conversion-type-id");
	node->Add(move(specifiers));
	for (;;)
	{
		ParseNodePtr op = ParsePtrOperator();
		if (!op)
			break;
		node->Add(move(op));
	}
	return node;
}

// literal-operator-id: KW_OPERATOR ST_EMPTYSTR TT_IDENTIFIER
ParseNodePtr Parser::ParseLiteralOperatorId()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_OPERATOR);
	if (!kw)
		return ParseNodePtr();
	if (!AtLiteral() || !Peek().HasFlag(PTF_ST_EMPTYSTR) || !AtIdentifier(1))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("literal-operator-id");
	node->Add(move(kw));
	node->Add(MatchLiteralLeaf());
	node->Add(MatchIdentifierLeaf());
	return node;
}

ParseNodePtr Parser::ParseTemplateArgumentList()
{
	return ParseCommaList("template-argument-list",
	                      &Parser::ParseTemplateArgumentDots);
}

ParseNodePtr Parser::ParseTemplateArgumentDots()
{
	return ParseDotsItem("template-argument-dots",
	                     &Parser::ParseTemplateArgument);
}

bool Parser::AtTemplateArgumentFollow() const
{
	return AtSimple(OP_COMMA) || AtSimple(OP_DOTS) || AtSimple(OP_GT) ||
	       Peek().kind == PTOK_RSHIFT_1 || Peek().kind == PTOK_RSHIFT_2;
}

// template-argument: constant-expression | type-id | id-expression
// The type reading wins (8.2/3, 14.3); every alternative must stop at
// a list boundary before committing so `TC1<C+1>` backs out of the
// type-id reading and reparses as a constant-expression.
ParseNodePtr Parser::ParseTemplateArgument()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("template-argument");
	ParseNodePtr argument = ParseTypeId();
	if (argument && AtTemplateArgumentFollow())
	{
		node->Add(move(argument));
		return node;
	}
	Restore(state);
	argument = ParseConstantExpression();
	if (argument && AtTemplateArgumentFollow())
	{
		node->Add(move(argument));
		return node;
	}
	Restore(state);
	argument = ParseIdExpression();
	if (argument && AtTemplateArgumentFollow())
	{
		node->Add(move(argument));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}
