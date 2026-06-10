// Declarators: named and abstract declarators, ptr-operators,
// parameter clauses (with the 8.2/7 abstract-first resolution),
// type-ids, initializers, function bodies, and exception
// specifications.

#include "parser.h"

using std::move;

// init-declarator: declarator initializer?
ParseNodePtr Parser::ParseInitDeclarator()
{
	ParseNodePtr declarator = ParseDeclarator();
	if (!declarator)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("init-declarator");
	node->Add(move(declarator));
	ParseNodePtr initializer = ParseInitializer();
	if (initializer)
		node->Add(move(initializer));
	return node;
}

// declarator: ptr-declarator | noptr-declarator trailing-return-type
// (the trailing-return form first so `auto f() -> int` keeps its
// arrow)
ParseNodePtr Parser::ParseDeclarator()
{
	return MemoParse(kMemoDeclarator, &Parser::ParseDeclaratorRule);
}

ParseNodePtr Parser::ParseDeclaratorRule()
{
	State state = Save();
	ParseNodePtr noptr = ParseNoptrDeclarator();
	if (noptr)
	{
		ParseNodePtr trailing = ParseTrailingReturnType();
		if (trailing)
		{
			ParseNodePtr node = MakeParseNode("declarator");
			node->Add(move(noptr));
			node->Add(move(trailing));
			return node;
		}
		Restore(state);
	}
	ParseNodePtr ptr = ParsePtrDeclarator();
	if (!ptr)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("declarator");
	node->Add(move(ptr));
	return node;
}

// ptr-declarator: ptr-operator* noptr-declarator
// Greedy ptr-operators are safe: a failed ParsePtrOperator consumes
// nothing, and no noptr-declarator starts with one.
ParseNodePtr Parser::ParsePtrDeclarator()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("ptr-declarator");
	for (;;)
	{
		ParseNodePtr op = ParsePtrOperator();
		if (!op)
			break;
		node->Add(move(op));
	}
	ParseNodePtr noptr = ParseNoptrDeclarator();
	if (!noptr)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(noptr));
	return node;
}

// noptr-declarator: (declarator-id attribute-specifier* |
//     OP_LPAREN ptr-declarator OP_RPAREN) noptr-declarator-suffix*
ParseNodePtr Parser::ParseNoptrDeclarator()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("noptr-declarator");
	if (AtSimple(OP_LPAREN))
	{
		Advance();
		ParseNodePtr inner = ParsePtrDeclarator();
		if (!inner || !MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		ParseNodePtr group = MakeParseNode("declarator-group");
		group->Add(move(inner));
		node->Add(move(group));
	}
	else
	{
		ParseNodePtr id = ParseDeclaratorId();
		if (!id)
			return ParseNodePtr();
		node->Add(move(id));
		ParseAttributeSpecifiers(node.get());
	}
	for (;;)
	{
		ParseNodePtr suffix = ParseNoptrDeclaratorSuffix();
		if (!suffix)
			break;
		node->Add(move(suffix));
	}
	return node;
}

// noptr-declarator-suffix: parameters-and-qualifiers |
//     OP_LSQUARE constant-expression? OP_RSQUARE attribute-specifier*
ParseNodePtr Parser::ParseNoptrDeclaratorSuffix()
{
	if (AtSimple(OP_LPAREN))
		return ParseParametersAndQualifiers();
	if (!AtSimple(OP_LSQUARE))
		return ParseNodePtr();
	State state = Save();
	Advance();
	ParseNodePtr node = MakeParseNode("array-suffix");
	if (!AtSimple(OP_RSQUARE))
	{
		ParseNodePtr bound = ParseConstantExpression();
		if (!bound)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(bound));
	}
	if (!MatchSimple(OP_RSQUARE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseAttributeSpecifiers(node.get());
	return node;
}

// parameters-and-qualifiers: OP_LPAREN parameter-declaration-clause
//     OP_RPAREN cv-qualifier* ref-qualifier? exception-specification?
//     attribute-specifier*
ParseNodePtr Parser::ParseParametersAndQualifiers()
{
	return MemoParse(kMemoParametersAndQualifiers,
	                 &Parser::ParseParametersAndQualifiersRule);
}

ParseNodePtr Parser::ParseParametersAndQualifiersRule()
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return ParseNodePtr();
	ParseNodePtr parameters = ParseParameterDeclarationClause();
	if (!parameters || !MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("parameters-and-qualifiers");
	node->Add(move(parameters));
	for (;;)
	{
		ParseNodePtr cv = ParseCvQualifier();
		if (!cv)
			break;
		node->Add(move(cv));
	}
	ParseNodePtr ref = ParseRefQualifier();
	if (ref)
		node->Add(move(ref));
	ParseNodePtr exception = ParseExceptionSpecification();
	if (exception)
		node->Add(move(exception));
	ParseAttributeSpecifiers(node.get());
	return node;
}

// trailing-return-type: OP_ARROW trailing-type-specifier-seq
//     abstract-declarator?
ParseNodePtr Parser::ParseTrailingReturnType()
{
	State state = Save();
	if (!MatchSimple(OP_ARROW))
		return ParseNodePtr();
	ParseNodePtr specifiers = ParseTrailingTypeSpecifierSeq();
	if (!specifiers)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("trailing-return-type");
	node->Add(move(specifiers));
	ParseNodePtr declarator = ParseAbstractDeclarator();
	if (declarator)
		node->Add(move(declarator));
	return node;
}

// ptr-operator: OP_STAR attribute-specifier* cv-qualifier* |
//     (OP_AMP | OP_LAND) attribute-specifier* |
//     nested-name-specifier OP_STAR attribute-specifier* cv-qualifier*
ParseNodePtr Parser::ParsePtrOperator()
{
	ParseNodePtr node = MakeParseNode("ptr-operator");
	if (AtSimple(OP_STAR))
	{
		node->Add(MatchSimpleLeaf(OP_STAR));
		ParseAttributeSpecifiers(node.get());
		for (;;)
		{
			ParseNodePtr cv = ParseCvQualifier();
			if (!cv)
				break;
			node->Add(move(cv));
		}
		return node;
	}
	if (AtSimple(OP_AMP) || AtSimple(OP_LAND))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		ParseAttributeSpecifiers(node.get());
		return node;
	}
	State state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested && AtSimple(OP_STAR))
	{
		node->Add(move(nested));
		node->Add(MatchSimpleLeaf(OP_STAR));
		ParseAttributeSpecifiers(node.get());
		for (;;)
		{
			ParseNodePtr cv = ParseCvQualifier();
			if (!cv)
				break;
			node->Add(move(cv));
		}
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

ParseNodePtr Parser::ParseCvQualifier()
{
	if (!AtSimple(KW_CONST) && !AtSimple(KW_VOLATILE))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("cv-qualifier");
	node->Add(MakeTokenLeaf(Peek()));
	Advance();
	return node;
}

ParseNodePtr Parser::ParseRefQualifier()
{
	if (!AtSimple(OP_AMP) && !AtSimple(OP_LAND))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("ref-qualifier");
	node->Add(MakeTokenLeaf(Peek()));
	Advance();
	return node;
}

// declarator-id: OP_DOTS? id-expression
ParseNodePtr Parser::ParseDeclaratorId()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("declarator-id");
	if (AtSimple(OP_DOTS))
		node->Add(MatchSimpleLeaf(OP_DOTS));
	ParseNodePtr id = ParseIdExpression();
	if (!id)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(id));
	return node;
}

// type-id: type-specifier-seq abstract-declarator?
ParseNodePtr Parser::ParseTypeId()
{
	return MemoParse(kMemoTypeId, &Parser::ParseTypeIdRule);
}

ParseNodePtr Parser::ParseTypeIdRule()
{
	ParseNodePtr specifiers = ParseTypeSpecifierSeq();
	if (!specifiers)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("type-id");
	node->Add(move(specifiers));
	ParseNodePtr declarator = ParseAbstractDeclarator();
	if (declarator)
		node->Add(move(declarator));
	return node;
}

// abstract-declarator: ptr-abstract-declarator |
//     noptr-abstract-declarator? trailing-return-type |
//     abstract-pack-declarator
ParseNodePtr Parser::ParseAbstractDeclarator()
{
	return MemoParse(kMemoAbstractDeclarator,
	                 &Parser::ParseAbstractDeclaratorRule);
}

ParseNodePtr Parser::ParseAbstractDeclaratorRule()
{
	State state = Save();
	ParseNodePtr noptr = ParseNoptrAbstractDeclarator();
	ParseNodePtr trailing = ParseTrailingReturnType();
	if (trailing)
	{
		ParseNodePtr node = MakeParseNode("abstract-declarator");
		if (noptr)
			node->Add(move(noptr));
		node->Add(move(trailing));
		return node;
	}
	Restore(state);
	// abstract-pack-declarator before ptr-abstract-declarator: both
	// start with ptr-operator*, and the pack form consumes the OP_DOTS
	// that would otherwise satisfy the parameter FOLLOW check and
	// orphan a following parameter name (`const C&... mixins`).
	ParseNodePtr pack = ParseAbstractPackDeclarator();
	if (pack)
	{
		ParseNodePtr node = MakeParseNode("abstract-declarator");
		node->Add(move(pack));
		return node;
	}
	ParseNodePtr ptr = ParsePtrAbstractDeclarator();
	if (!ptr)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("abstract-declarator");
	node->Add(move(ptr));
	return node;
}

// ptr-abstract-declarator: ptr-operator* noptr-abstract-declarator |
//     ptr-operator+
ParseNodePtr Parser::ParsePtrAbstractDeclarator()
{
	ParseNodePtr node = MakeParseNode("ptr-abstract-declarator");
	int num_ops = 0;
	for (;;)
	{
		ParseNodePtr op = ParsePtrOperator();
		if (!op)
			break;
		node->Add(move(op));
		num_ops++;
	}
	ParseNodePtr noptr = ParseNoptrAbstractDeclarator();
	if (noptr)
		node->Add(move(noptr));
	else if (num_ops == 0)
		return ParseNodePtr();
	return node;
}

// noptr-abstract-declarator: (noptr-declarator-suffix |
//     OP_LPAREN ptr-abstract-declarator OP_RPAREN)
//     noptr-declarator-suffix*
// A `(` tries the parameter-clause suffix before the grouping form, so
// `(int)` is a parameter list while `(*)(int)` groups.
ParseNodePtr Parser::ParseNoptrAbstractDeclarator()
{
	ParseNodePtr node = MakeParseNode("noptr-abstract-declarator");
	ParseNodePtr root = ParseNoptrDeclaratorSuffix();
	if (!root && AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		ParseNodePtr inner = ParsePtrAbstractDeclarator();
		if (inner && MatchSimple(OP_RPAREN))
		{
			root = MakeParseNode("declarator-group");
			root->Add(move(inner));
		}
		else
			Restore(state);
	}
	if (!root)
		return ParseNodePtr();
	node->Add(move(root));
	for (;;)
	{
		ParseNodePtr suffix = ParseNoptrDeclaratorSuffix();
		if (!suffix)
			break;
		node->Add(move(suffix));
	}
	return node;
}

// abstract-pack-declarator: ptr-operator*
//     OP_DOTS noptr-declarator-suffix*
ParseNodePtr Parser::ParseAbstractPackDeclarator()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("abstract-pack-declarator");
	for (;;)
	{
		ParseNodePtr op = ParsePtrOperator();
		if (!op)
			break;
		node->Add(move(op));
	}
	if (!AtSimple(OP_DOTS))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(MatchSimpleLeaf(OP_DOTS));
	for (;;)
	{
		ParseNodePtr suffix = ParseNoptrDeclaratorSuffix();
		if (!suffix)
			break;
		node->Add(move(suffix));
	}
	return node;
}

// parameter-declaration-clause: parameter-declaration-list? OP_DOTS? |
//     parameter-declaration-list OP_COMMA OP_DOTS
ParseNodePtr Parser::ParseParameterDeclarationClause()
{
	ParseNodePtr node = MakeParseNode("parameter-declaration-clause");
	ParseNodePtr list = ParseCommaList("parameter-declaration-list",
	                                   &Parser::ParseParameterDeclaration);
	if (list)
	{
		node->Add(move(list));
		State state = Save();
		if (MatchSimple(OP_COMMA))
		{
			ParseNodePtr dots = MatchSimpleLeaf(OP_DOTS);
			if (dots)
			{
				node->Add(move(dots));
				return node;
			}
			Restore(state);
		}
	}
	if (AtSimple(OP_DOTS))
		node->Add(MatchSimpleLeaf(OP_DOTS));
	return node;
}

bool Parser::AtParameterDeclarationFollow() const
{
	return AtSimple(OP_COMMA) || AtSimple(OP_RPAREN) || AtSimple(OP_DOTS) ||
	       AtSimple(OP_ASS) || AtSimple(OP_GT) ||
	       Peek().kind == PTOK_RSHIFT_1 || Peek().kind == PTOK_RSHIFT_2;
}

// parameter-declaration: attribute-specifier* decl-specifier-seq
//     (declarator | abstract-declarator?) (OP_ASS initializer-clause)?
// The abstract (type) reading is tried first so a parenthesized
// type-name declares a function type rather than a name with redundant
// parens (8.2/7); each reading must stop at a clause boundary. The
// close-angle tokens are in the FOLLOW set because template-parameter
// reuses this parse.
ParseNodePtr Parser::ParseParameterDeclaration()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("parameter-declaration");
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr specifiers = ParseDeclSpecifierSeq();
	if (!specifiers)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(specifiers));
	State declarator_state = Save();
	ParseNodePtr declarator = ParseAbstractDeclarator();
	if (!declarator || !AtParameterDeclarationFollow())
	{
		Restore(declarator_state);
		declarator = ParseDeclarator();
		if (!declarator || !AtParameterDeclarationFollow())
		{
			Restore(declarator_state);
			declarator.reset();
		}
	}
	if (declarator)
		node->Add(move(declarator));
	if (!AtParameterDeclarationFollow())
	{
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(OP_ASS))
	{
		State default_state = Save();
		MatchSimple(OP_ASS);
		ParseNodePtr value = ParseInitializerClause();
		if (!value)
		{
			Restore(default_state);
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(value));
	}
	return node;
}

// function-definition: attribute-specifier* decl-specifier-seq
//     declarator virt-specifier* function-body
ParseNodePtr Parser::ParseFunctionDefinition()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("function-definition");
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr specifiers = ParseDeclSpecifierSeq();
	ParseNodePtr declarator;
	if (!specifiers || !(declarator = ParseDeclarator()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(specifiers));
	node->Add(move(declarator));
	for (;;)
	{
		ParseNodePtr virt = ParseVirtSpecifier();
		if (!virt)
			break;
		node->Add(move(virt));
	}
	ParseNodePtr body = ParseFunctionBody();
	if (!body)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(body));
	return node;
}

// function-body: ctor-initializer? compound-statement |
//     function-try-block | OP_ASS KW_DEFAULT OP_SEMICOLON |
//     OP_ASS KW_DELETE OP_SEMICOLON
ParseNodePtr Parser::ParseFunctionBody()
{
	ParseNodePtr node = MakeParseNode("function-body");
	if (AtSimple(OP_ASS))
	{
		State state = Save();
		MatchSimple(OP_ASS);
		if (AtSimple(KW_DEFAULT) || AtSimple(KW_DELETE))
		{
			node->Add(MakeTokenLeaf(Peek()));
			Advance();
			if (MatchSimple(OP_SEMICOLON))
				return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_TRY))
	{
		ParseNodePtr try_block = ParseFunctionTryBlock();
		if (!try_block)
			return ParseNodePtr();
		node->Add(move(try_block));
		return node;
	}
	State state = Save();
	ParseNodePtr ctor_initializer = ParseCtorInitializer();
	if (ctor_initializer)
		node->Add(move(ctor_initializer));
	ParseNodePtr body = ParseCompoundStatement();
	if (!body)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(body));
	return node;
}

// function-try-block: KW_TRY ctor-initializer? compound-statement
//     handler+
ParseNodePtr Parser::ParseFunctionTryBlock()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_TRY);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("function-try-block");
	node->Add(move(kw));
	ParseNodePtr ctor_initializer = ParseCtorInitializer();
	if (ctor_initializer)
		node->Add(move(ctor_initializer));
	ParseNodePtr body = ParseCompoundStatement();
	ParseNodePtr first_handler;
	if (!body || !(first_handler = ParseHandler()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(body));
	node->Add(move(first_handler));
	for (;;)
	{
		ParseNodePtr handler = ParseHandler();
		if (!handler)
			break;
		node->Add(move(handler));
	}
	return node;
}

// initializer: brace-or-equal-initializer |
//     OP_LPAREN expression-list OP_RPAREN
ParseNodePtr Parser::ParseInitializer()
{
	if (AtSimple(OP_ASS) || AtSimple(OP_LBRACE))
		return ParseBraceOrEqualInitializer();
	if (!AtSimple(OP_LPAREN))
		return ParseNodePtr();
	State state = Save();
	Advance();
	ParseNodePtr arguments = ParseExpressionList();
	if (!arguments || !MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("initializer");
	node->Add(move(arguments));
	return node;
}

// brace-or-equal-initializer: OP_ASS initializer-clause |
//     braced-init-list
ParseNodePtr Parser::ParseBraceOrEqualInitializer()
{
	ParseNodePtr node = MakeParseNode("brace-or-equal-initializer");
	if (AtSimple(OP_ASS))
	{
		State state = Save();
		MatchSimple(OP_ASS);
		ParseNodePtr value = ParseInitializerClause();
		if (!value)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(value));
		return node;
	}
	ParseNodePtr braced = ParseBracedInitList();
	if (!braced)
		return ParseNodePtr();
	node->Add(move(braced));
	return node;
}

// exception-specification: dynamic-exception-specification |
//     noexcept-specification
ParseNodePtr Parser::ParseExceptionSpecification()
{
	if (AtSimple(KW_THROW))
	{
		State state = Save();
		ParseNodePtr kw = MatchSimpleLeaf(KW_THROW);
		if (!MatchSimple(OP_LPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		ParseNodePtr node = MakeParseNode("dynamic-exception-specification");
		node->Add(move(kw));
		if (!AtSimple(OP_RPAREN))
		{
			ParseNodePtr types =
				ParseCommaList("type-id-list", &Parser::ParseTypeIdDots);
			if (!types)
			{
				Restore(state);
				return ParseNodePtr();
			}
			node->Add(move(types));
		}
		if (!MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		return node;
	}
	if (AtSimple(KW_NOEXCEPT))
	{
		ParseNodePtr node = MakeParseNode("noexcept-specification");
		node->Add(MatchSimpleLeaf(KW_NOEXCEPT));
		State state = Save();
		if (MatchSimple(OP_LPAREN))
		{
			ParseNodePtr value = ParseConstantExpression();
			if (value && MatchSimple(OP_RPAREN))
				node->Add(move(value));
			else
				Restore(state);
		}
		return node;
	}
	return ParseNodePtr();
}

ParseNodePtr Parser::ParseTypeIdDots()
{
	return ParseDotsItem("type-id-dots", &Parser::ParseTypeId);
}
