#include "ast/ast_parser.h"

using std::string;
using std::move;

namespace {

AstStmtPtr MakeStmt(EStmtKind kind)
{
	return AstStmtPtr(new AstStmt(kind));
}

AstDeclPtr MakeDecl(EDeclKind kind)
{
	return AstDeclPtr(new AstDecl(kind));
}

}  // namespace

// block-item: declaration before statement (6.8: anything that can be
// a declaration is one).
AstStmtPtr AstParser::ParseBlockItem()
{
	AstDeclPtr decl = ParseDeclaration();
	if (decl)
	{
		AstStmtPtr stmt = MakeStmt(SK_DECLARATION);
		stmt->decl = move(decl);
		return stmt;
	}
	return ParseStatement();
}

AstStmtPtr AstParser::ParseCompoundStatement()
{
	State state = Save();
	if (!MatchSimple(OP_LBRACE))
		return AstStmtPtr();
	PushTransientScope();
	AstStmtPtr stmt = MakeStmt(SK_COMPOUND);
	while (!AtSimple(OP_RBRACE))
	{
		AstStmtPtr item = ParseBlockItem();
		if (!item)
		{
			PopScope();
			Restore(state);
			return AstStmtPtr();
		}
		stmt->items.push_back(move(item));
	}
	PopScope();
	Advance();  // OP_RBRACE
	return stmt;
}

AstStmtPtr AstParser::ParseLabeledStatement()
{
	State state = Save();
	if (AtSimple(KW_CASE))
	{
		Advance();
		AstExprPtr value = ParseExpression();
		if (!value || !MatchSimple(OP_COLON))
		{
			Restore(state);
			return AstStmtPtr();
		}
		// 6.8: the labeled statement may itself be a declaration.
		AstStmtPtr body = ParseBlockItem();
		if (!body)
		{
			Restore(state);
			return AstStmtPtr();
		}
		AstStmtPtr stmt = MakeStmt(SK_CASE);
		stmt->expr = move(value);
		stmt->body = move(body);
		return stmt;
	}
	if (AtSimple(KW_DEFAULT))
	{
		Advance();
		if (!MatchSimple(OP_COLON))
		{
			Restore(state);
			return AstStmtPtr();
		}
		AstStmtPtr body = ParseBlockItem();
		if (!body)
		{
			Restore(state);
			return AstStmtPtr();
		}
		AstStmtPtr stmt = MakeStmt(SK_DEFAULT);
		stmt->body = move(body);
		return stmt;
	}
	if (AtIdentifier() && AtSimple(OP_COLON, 1))
	{
		string label = Peek().spelling;
		Advance();
		Advance();
		// 6.1/6.7: the labeled statement may itself be a declaration.
		AstStmtPtr body = ParseBlockItem();
		if (!body)
		{
			Restore(state);
			return AstStmtPtr();
		}
		AstStmtPtr stmt = MakeStmt(SK_LABELED);
		stmt->label = label;
		stmt->body = move(body);
		return stmt;
	}
	return AstStmtPtr();
}

// condition: the declaration form requires its initializer, so
// `if (int x)` correctly fails over to (and then with) the
// expression reading.
AstConditionPtr AstParser::ParseCondition()
{
	State state = Save();
	AstConditionPtr condition(new AstCondition());
	if (ParseSpecifierSeq(condition->specifiers, kDeclSpecifierSeq))
	{
		AstDeclaratorPtr declarator;
		AstInitializerPtr init;
		if (ParseDeclarator(declarator, true) &&
		    ParseBraceOrEqualInitializer(init))
		{
			condition->is_declaration = true;
			condition->declarator = move(declarator);
			condition->init = move(init);
			const AstName* id = condition->declarator->IdName();
			if (id && id->IsPlainIdentifier())
				Register(id->parts[0].identifier, NF_VALUE);
			return condition;
		}
	}
	Restore(state);
	condition->specifiers.clear();
	condition->expr = ParseExpression();
	if (!condition->expr)
	{
		Restore(state);
		return AstConditionPtr();
	}
	return condition;
}

AstStmtPtr AstParser::ParseSelectionStatement()
{
	State state = Save();
	bool is_if = AtSimple(KW_IF);
	if (!is_if && !AtSimple(KW_SWITCH))
		return AstStmtPtr();
	Advance();
	PushTransientScope();
	AstConditionPtr condition;
	AstStmtPtr body;
	if (!MatchSimple(OP_LPAREN) || !(condition = ParseCondition()) ||
	    !MatchSimple(OP_RPAREN) || !(body = ParseStatement()))
	{
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	AstStmtPtr stmt = MakeStmt(is_if ? SK_IF : SK_SWITCH);
	stmt->condition = move(condition);
	if (is_if)
	{
		stmt->then_branch = move(body);
		if (AtSimple(KW_ELSE))
		{
			Advance();
			stmt->else_branch = ParseStatement();
			if (!stmt->else_branch)
			{
				PopScope();
				Restore(state);
				return AstStmtPtr();
			}
		}
	}
	else
		stmt->body = move(body);
	PopScope();
	return stmt;
}

// for-init-statement: a simple-declaration (with its semicolon) or an
// optional expression followed by one.
AstStmtPtr AstParser::ParseForInitStatement()
{
	AstDeclPtr decl = ParseSimpleDeclaration();
	if (decl)
	{
		AstStmtPtr stmt = MakeStmt(SK_DECLARATION);
		stmt->decl = move(decl);
		return stmt;
	}
	State state = Save();
	AstExprPtr expr;
	if (!AtSimple(OP_SEMICOLON))
	{
		expr = ParseExpression();
		if (!expr)
			return AstStmtPtr();
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstStmtPtr();
	}
	AstStmtPtr stmt = MakeStmt(SK_EXPRESSION);
	stmt->expr = move(expr);
	return stmt;
}

AstStmtPtr AstParser::ParseIterationStatement()
{
	State state = Save();
	if (AtSimple(KW_WHILE))
	{
		Advance();
		PushTransientScope();
		AstConditionPtr condition;
		AstStmtPtr body;
		if (MatchSimple(OP_LPAREN) && (condition = ParseCondition()) &&
		    MatchSimple(OP_RPAREN) && (body = ParseStatement()))
		{
			PopScope();
			AstStmtPtr stmt = MakeStmt(SK_WHILE);
			stmt->condition = move(condition);
			stmt->body = move(body);
			return stmt;
		}
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	if (AtSimple(KW_DO))
	{
		Advance();
		AstStmtPtr body = ParseStatement();
		AstExprPtr expr;
		if (body && MatchSimple(KW_WHILE) && MatchSimple(OP_LPAREN) &&
		    (expr = ParseExpression()) && MatchSimple(OP_RPAREN) &&
		    MatchSimple(OP_SEMICOLON))
		{
			AstStmtPtr stmt = MakeStmt(SK_DO);
			stmt->body = move(body);
			stmt->condition = AstConditionPtr(new AstCondition());
			stmt->condition->expr = move(expr);
			return stmt;
		}
		Restore(state);
		return AstStmtPtr();
	}
	if (!AtSimple(KW_FOR))
		return AstStmtPtr();
	Advance();
	PushTransientScope();
	if (!MatchSimple(OP_LPAREN))
	{
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	// 6.5.4: for ( for-range-declaration : for-range-initializer )
	// statement. The range declaration is a specifier-seq plus one
	// uninitialized declarator followed by a colon.
	{
		State range_state = Save();
		AstDeclPtr range_decl = MakeDecl(DK_SIMPLE);
		AstInitDeclarator range_declarator;
		range_declarator.begin_token = pos_;
		if (ParseSpecifierSeq(range_decl->specifiers,
		                      kDeclSpecifierSeq) &&
		    ParseDeclarator(range_declarator.declarator, true) &&
		    AtSimple(OP_COLON))
		{
			Advance();
			const AstName* id =
				range_declarator.declarator->IdName();
			if (id && id->IsPlainIdentifier())
				Register(id->parts[0].identifier, NF_VALUE);
			range_decl->declarators.push_back(
				move(range_declarator));
			AstExprPtr range_init = AtSimple(OP_LBRACE)
				? ParseBracedInitList() : ParseExpression();
			AstStmtPtr range_body;
			if (range_init && MatchSimple(OP_RPAREN) &&
			    (range_body = ParseStatement()))
			{
				PopScope();
				AstStmtPtr range_stmt = MakeStmt(SK_FOR);
				range_stmt->for_range_decl = move(range_decl);
				range_stmt->for_range_init = move(range_init);
				range_stmt->body = move(range_body);
				return range_stmt;
			}
			PopScope();
			Restore(state);
			return AstStmtPtr();
		}
		Restore(range_state);
	}
	AstStmtPtr init;
	if (!(init = ParseForInitStatement()))
	{
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	AstStmtPtr stmt = MakeStmt(SK_FOR);
	if (init->kind != SK_EXPRESSION || init->expr)
		stmt->for_init = move(init);
	if (!AtSimple(OP_SEMICOLON))
	{
		stmt->condition = ParseCondition();
		if (!stmt->condition)
		{
			PopScope();
			Restore(state);
			return AstStmtPtr();
		}
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	if (!AtSimple(OP_RPAREN))
	{
		stmt->iteration = ParseExpression();
		if (!stmt->iteration)
		{
			PopScope();
			Restore(state);
			return AstStmtPtr();
		}
	}
	if (!MatchSimple(OP_RPAREN) || !(stmt->body = ParseStatement()))
	{
		PopScope();
		Restore(state);
		return AstStmtPtr();
	}
	PopScope();
	return stmt;
}

AstStmtPtr AstParser::ParseJumpStatement()
{
	State state = Save();
	if (AtSimple(KW_BREAK) || AtSimple(KW_CONTINUE))
	{
		bool is_break = AtSimple(KW_BREAK);
		Advance();
		if (!MatchSimple(OP_SEMICOLON))
		{
			Restore(state);
			return AstStmtPtr();
		}
		return MakeStmt(is_break ? SK_BREAK : SK_CONTINUE);
	}
	if (AtSimple(KW_GOTO))
	{
		Advance();
		if (!AtIdentifier())
		{
			Restore(state);
			return AstStmtPtr();
		}
		string label = Peek().spelling;
		Advance();
		if (!MatchSimple(OP_SEMICOLON))
		{
			Restore(state);
			return AstStmtPtr();
		}
		AstStmtPtr stmt = MakeStmt(SK_GOTO);
		stmt->label = label;
		return stmt;
	}
	if (AtSimple(KW_RETURN) || AtSimple(KW_THROW))
	{
		bool is_return = AtSimple(KW_RETURN);
		Advance();
		AstStmtPtr stmt = MakeStmt(is_return ? SK_RETURN : SK_THROW);
		if (!AtSimple(OP_SEMICOLON))
		{
			// 6.6.3p1: `return braced-init-list ;`.
			if (is_return && AtSimple(OP_LBRACE))
				stmt->expr = ParseBracedInitList();
			else
				stmt->expr = is_return ? ParseExpression()
				                       : ParseAssignmentExpression();
			if (!stmt->expr)
			{
				Restore(state);
				return AstStmtPtr();
			}
		}
		if (!MatchSimple(OP_SEMICOLON))
		{
			Restore(state);
			return AstStmtPtr();
		}
		return stmt;
	}
	return AstStmtPtr();
}

AstStmtPtr AstParser::ParseTryBlock()
{
	State state = Save();
	if (!MatchSimple(KW_TRY))
		return AstStmtPtr();
	AstStmtPtr body = ParseCompoundStatement();
	if (!body)
	{
		Restore(state);
		return AstStmtPtr();
	}
	AstStmtPtr stmt = MakeStmt(SK_TRY);
	stmt->body = move(body);
	while (AtSimple(KW_CATCH))
	{
		State handler_state = Save();
		Advance();
		PushTransientScope();
		AstHandler handler;
		bool ok = MatchSimple(OP_LPAREN);
		if (ok)
		{
			if (MatchSimple(OP_DOTS))
				handler.ellipsis = true;
			else
			{
				ok = ParseSpecifierSeq(handler.specifiers, kDeclSpecifierSeq);
				if (ok)
				{
					State decl_state = Save();
					AstDeclaratorPtr named;
					if (ParseDeclarator(named, true) && AtSimple(OP_RPAREN))
						handler.declarator = move(named);
					else
					{
						Restore(decl_state);
						AstDeclaratorPtr abstract;
						if (ParseDeclarator(abstract, false) &&
						    !abstract->Empty())
							handler.declarator = move(abstract);
					}
					if (handler.declarator)
					{
						const AstName* id = handler.declarator->IdName();
						if (id && id->IsPlainIdentifier())
							Register(id->parts[0].identifier, NF_VALUE);
					}
				}
			}
		}
		ok = ok && MatchSimple(OP_RPAREN);
		if (ok)
		{
			handler.body = ParseCompoundStatement();
			ok = handler.body != 0;
		}
		PopScope();
		if (!ok)
		{
			Restore(handler_state);
			break;
		}
		stmt->handlers.push_back(move(handler));
	}
	if (stmt->handlers.empty())
	{
		Restore(state);
		return AstStmtPtr();
	}
	return stmt;
}

AstStmtPtr AstParser::ParseExpressionStatement()
{
	State state = Save();
	AstStmtPtr stmt = MakeStmt(SK_EXPRESSION);
	if (!AtSimple(OP_SEMICOLON))
	{
		stmt->expr = ParseExpression();
		if (!stmt->expr)
		{
			Restore(state);
			return AstStmtPtr();
		}
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstStmtPtr();
	}
	return stmt;
}

AstStmtPtr AstParser::ParseStatement()
{
	if (AtSimple(OP_LBRACE))
		return ParseCompoundStatement();
	AstStmtPtr stmt = ParseLabeledStatement();
	if (stmt)
		return stmt;
	if (AtSimple(KW_IF) || AtSimple(KW_SWITCH))
		return ParseSelectionStatement();
	if (AtSimple(KW_WHILE) || AtSimple(KW_DO) || AtSimple(KW_FOR))
		return ParseIterationStatement();
	if (AtSimple(KW_BREAK) || AtSimple(KW_CONTINUE) || AtSimple(KW_GOTO) ||
	    AtSimple(KW_RETURN) || AtSimple(KW_THROW))
		return ParseJumpStatement();
	if (AtSimple(KW_TRY))
		return ParseTryBlock();
	return ParseExpressionStatement();
}
