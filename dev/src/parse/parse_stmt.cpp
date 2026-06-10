// Statements: the statement dispatcher (labeled and keyword forms by
// lookahead, then declaration-statement before expression-statement
// per 6.8), compound/selection/iteration/jump statements, conditions,
// and try-blocks with their handlers.

#include "parser.h"

using std::move;

ParseNodePtr Parser::ParseStatement()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("statement");
	ParseAttributeSpecifiers(node.get());
	// labeled-statement
	if (AtSimple(KW_CASE))
	{
		node->Add(MatchSimpleLeaf(KW_CASE));
		ParseNodePtr value = ParseConstantExpression();
		ParseNodePtr body;
		if (value && MatchSimple(OP_COLON) && (body = ParseStatement()))
		{
			node->Add(move(value));
			node->Add(move(body));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_DEFAULT))
	{
		node->Add(MatchSimpleLeaf(KW_DEFAULT));
		ParseNodePtr body;
		if (MatchSimple(OP_COLON) && (body = ParseStatement()))
		{
			node->Add(move(body));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtIdentifier() && AtSimple(OP_COLON, 1))
	{
		node->Add(MatchIdentifierLeaf());
		MatchSimple(OP_COLON);
		ParseNodePtr body = ParseStatement();
		if (body)
		{
			node->Add(move(body));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	// keyword-dispatched statement kinds
	ParseNodePtr inner;
	if (AtSimple(OP_LBRACE))
		inner = ParseCompoundStatement();
	else if (AtSimple(KW_IF) || AtSimple(KW_SWITCH))
		inner = ParseSelectionStatement();
	else if (AtSimple(KW_WHILE) || AtSimple(KW_DO) || AtSimple(KW_FOR))
		inner = ParseIterationStatement();
	else if (AtSimple(KW_BREAK) || AtSimple(KW_CONTINUE) ||
	         AtSimple(KW_RETURN) || AtSimple(KW_GOTO))
		inner = ParseJumpStatement();
	else if (AtSimple(KW_TRY))
		inner = ParseTryBlock();
	if (inner)
	{
		node->Add(move(inner));
		return node;
	}
	Restore(state);
	// declaration-statement before expression-statement (6.8: anything
	// that can be a declaration is a declaration); both parse their own
	// attribute prefixes and trailing semicolon.
	ParseNodePtr declaration = ParseBlockDeclaration();
	if (declaration)
	{
		ParseNodePtr wrapper = MakeParseNode("declaration-statement");
		wrapper->Add(move(declaration));
		return wrapper;
	}
	node = MakeParseNode("statement");
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr expression = ParseExpressionStatement();
	if (expression)
	{
		node->Add(move(expression));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// expression-statement: expression? OP_SEMICOLON
ParseNodePtr Parser::ParseExpressionStatement()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("expression-statement");
	if (!AtSimple(OP_SEMICOLON))
	{
		ParseNodePtr expression = ParseExpression();
		if (!expression)
			return ParseNodePtr();
		node->Add(move(expression));
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// compound-statement: OP_LBRACE statement* OP_RBRACE
ParseNodePtr Parser::ParseCompoundStatement()
{
	State state = Save();
	if (!MatchSimple(OP_LBRACE))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("compound-statement");
	for (;;)
	{
		ParseNodePtr statement = ParseStatement();
		if (!statement)
			break;
		node->Add(move(statement));
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// selection-statement: KW_IF OP_LPAREN condition OP_RPAREN statement
//     (KW_ELSE statement)? | KW_SWITCH OP_LPAREN condition OP_RPAREN
//     statement
// The greedy else attaches to the innermost if.
ParseNodePtr Parser::ParseSelectionStatement()
{
	State state = Save();
	bool is_if = AtSimple(KW_IF);
	if (!is_if && !AtSimple(KW_SWITCH))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("selection-statement");
	node->Add(MakeTokenLeaf(Peek()));
	Advance();
	ParseNodePtr condition;
	ParseNodePtr body;
	if (!MatchSimple(OP_LPAREN) || !(condition = ParseCondition()) ||
	    !MatchSimple(OP_RPAREN) || !(body = ParseStatement()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(condition));
	node->Add(move(body));
	if (is_if && AtSimple(KW_ELSE))
	{
		ParseNodePtr else_kw = MatchSimpleLeaf(KW_ELSE);
		ParseNodePtr else_body = ParseStatement();
		if (!else_body)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(else_kw));
		node->Add(move(else_body));
	}
	return node;
}

// condition: condition-declaration | expression (the declaration
// reading wins, 6.4)
ParseNodePtr Parser::ParseCondition()
{
	ParseNodePtr node = MakeParseNode("condition");
	ParseNodePtr inner = ParseConditionDeclaration();
	if (!inner)
		inner = ParseExpression();
	if (!inner)
		return ParseNodePtr();
	node->Add(move(inner));
	return node;
}

// condition-declaration: attribute-specifier* decl-specifier-seq
//     declarator (OP_ASS initializer-clause | braced-init-list)
ParseNodePtr Parser::ParseConditionDeclaration()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("condition-declaration");
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
	ParseNodePtr value;
	if (MatchSimple(OP_ASS))
		value = ParseInitializerClause();
	else
		value = ParseBracedInitList();
	if (!value)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(value));
	return node;
}

// iteration-statement: while / do-while / the two for forms
ParseNodePtr Parser::ParseIterationStatement()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("iteration-statement");
	if (AtSimple(KW_WHILE))
	{
		node->Add(MatchSimpleLeaf(KW_WHILE));
		ParseNodePtr condition;
		ParseNodePtr body;
		if (MatchSimple(OP_LPAREN) && (condition = ParseCondition()) &&
		    MatchSimple(OP_RPAREN) && (body = ParseStatement()))
		{
			node->Add(move(condition));
			node->Add(move(body));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_DO))
	{
		node->Add(MatchSimpleLeaf(KW_DO));
		ParseNodePtr body = ParseStatement();
		ParseNodePtr condition;
		if (body && MatchSimple(KW_WHILE) && MatchSimple(OP_LPAREN) &&
		    (condition = ParseExpression()) && MatchSimple(OP_RPAREN) &&
		    MatchSimple(OP_SEMICOLON))
		{
			node->Add(move(body));
			node->Add(move(condition));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (!AtSimple(KW_FOR))
		return ParseNodePtr();
	node->Add(MatchSimpleLeaf(KW_FOR));
	if (!MatchSimple(OP_LPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr suffix = ParseForSuffix();
	ParseNodePtr body;
	if (!suffix || !MatchSimple(OP_RPAREN) || !(body = ParseStatement()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(suffix));
	node->Add(move(body));
	return node;
}

// The part of a for statement between ( and ): either
// for-init-statement condition? OP_SEMICOLON expression? or
// for-range-declaration OP_COLON for-range-initializer.
ParseNodePtr Parser::ParseForSuffix()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("for-classic");
	ParseNodePtr init = ParseForInitStatement();
	if (init)
	{
		node->Add(move(init));
		ParseNodePtr condition;
		if (!AtSimple(OP_SEMICOLON))
			condition = ParseCondition();
		bool condition_ok = condition || AtSimple(OP_SEMICOLON);
		if (condition_ok && MatchSimple(OP_SEMICOLON))
		{
			if (condition)
				node->Add(move(condition));
			if (!AtSimple(OP_RPAREN))
			{
				ParseNodePtr step = ParseExpression();
				if (step && AtSimple(OP_RPAREN))
				{
					node->Add(move(step));
					return node;
				}
			}
			else
				return node;
		}
	}
	// for-range
	Restore(state);
	node = MakeParseNode("for-range");
	ParseNodePtr declaration = ParseForRangeDeclaration();
	ParseNodePtr initializer;
	if (!declaration || !MatchSimple(OP_COLON) ||
	    !(initializer = ParseForRangeInitializer()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(declaration));
	node->Add(move(initializer));
	return node;
}

// for-init-statement: simple-declaration before expression-statement
// (6.8)
ParseNodePtr Parser::ParseForInitStatement()
{
	ParseNodePtr init = ParseSimpleDeclaration();
	if (!init)
		init = ParseExpressionStatement();
	if (!init)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("for-init-statement");
	node->Add(move(init));
	return node;
}

// for-range-declaration: attribute-specifier* decl-specifier-seq
//     declarator
ParseNodePtr Parser::ParseForRangeDeclaration()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("for-range-declaration");
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
	return node;
}

// for-range-initializer: expression | braced-init-list
ParseNodePtr Parser::ParseForRangeInitializer()
{
	ParseNodePtr value;
	if (AtSimple(OP_LBRACE))
		value = ParseBracedInitList();
	else
		value = ParseExpression();
	if (!value)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("for-range-initializer");
	node->Add(move(value));
	return node;
}

// jump-statement: break/continue/return/goto forms
ParseNodePtr Parser::ParseJumpStatement()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("jump-statement");
	if (AtSimple(KW_BREAK) || AtSimple(KW_CONTINUE))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		if (MatchSimple(OP_SEMICOLON))
			return node;
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_RETURN))
	{
		node->Add(MatchSimpleLeaf(KW_RETURN));
		if (AtSimple(OP_LBRACE))
		{
			ParseNodePtr value = ParseBracedInitList();
			if (value && MatchSimple(OP_SEMICOLON))
			{
				node->Add(move(value));
				return node;
			}
			Restore(state);
			return ParseNodePtr();
		}
		if (!AtSimple(OP_SEMICOLON))
		{
			ParseNodePtr value = ParseExpression();
			if (!value)
			{
				Restore(state);
				return ParseNodePtr();
			}
			node->Add(move(value));
		}
		if (MatchSimple(OP_SEMICOLON))
			return node;
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_GOTO))
	{
		node->Add(MatchSimpleLeaf(KW_GOTO));
		ParseNodePtr label = MatchIdentifierLeaf();
		if (label && MatchSimple(OP_SEMICOLON))
		{
			node->Add(move(label));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	return ParseNodePtr();
}

// try-block: KW_TRY compound-statement handler+
ParseNodePtr Parser::ParseTryBlock()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_TRY);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("try-block");
	node->Add(move(kw));
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

// handler: KW_CATCH OP_LPAREN exception-declaration OP_RPAREN
//     compound-statement
ParseNodePtr Parser::ParseHandler()
{
	State state = Save();
	ParseNodePtr kw = MatchSimpleLeaf(KW_CATCH);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("handler");
	node->Add(move(kw));
	ParseNodePtr declaration;
	ParseNodePtr body;
	if (!MatchSimple(OP_LPAREN) ||
	    !(declaration = ParseExceptionDeclaration()) ||
	    !MatchSimple(OP_RPAREN) || !(body = ParseCompoundStatement()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(declaration));
	node->Add(move(body));
	return node;
}

// exception-declaration: OP_DOTS | attribute-specifier*
//     type-specifier+ attribute-specifier* (declarator |
//     abstract-declarator?)
// The abstract (type) reading is tried first against FOLLOW = `)`
// (8.2/7), then the named declarator.
ParseNodePtr Parser::ParseExceptionDeclaration()
{
	ParseNodePtr node = MakeParseNode("exception-declaration");
	if (AtSimple(OP_DOTS))
	{
		node->Add(MatchSimpleLeaf(OP_DOTS));
		return node;
	}
	State state = Save();
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr specifiers = ParseTypeSpecifierSeq();
	if (!specifiers)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(specifiers));
	State declarator_state = Save();
	ParseNodePtr declarator = ParseAbstractDeclarator();
	if (declarator && AtSimple(OP_RPAREN))
	{
		node->Add(move(declarator));
		return node;
	}
	Restore(declarator_state);
	declarator = ParseDeclarator();
	if (declarator && AtSimple(OP_RPAREN))
	{
		node->Add(move(declarator));
		return node;
	}
	Restore(declarator_state);
	if (AtSimple(OP_RPAREN))
		return node;
	Restore(state);
	return ParseNodePtr();
}
