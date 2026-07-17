#include "ast/ast_parser.h"

#include "hosted_probes.h"

using std::string;
using std::vector;
using std::move;

namespace {

AstExprPtr MakeExpr(EExprKind kind)
{
	return AstExprPtr(new AstExpr(kind));
}

bool IsSimpleTypeKeyword(ETokenType type)
{
	switch (type)
	{
	case KW_BOOL: case KW_CHAR: case KW_CHAR16_T: case KW_CHAR32_T:
	case KW_DOUBLE: case KW_FLOAT: case KW_INT: case KW_LONG:
	case KW_SHORT: case KW_SIGNED: case KW_UNSIGNED: case KW_VOID:
	case KW_WCHAR_T:
		return true;
	default:
		return false;
	}
}

}  // namespace

// expression: assignment-expression (OP_COMMA assignment-expression)*
AstExprPtr AstParser::ParseExpression()
{
	AstExprPtr left = ParseAssignmentExpression();
	if (!left)
		return AstExprPtr();
	for (;;)
	{
		State state = Save();
		if (!MatchSimple(OP_COMMA))
			break;
		AstExprPtr right = ParseAssignmentExpression();
		if (!right)
		{
			Restore(state);
			break;
		}
		AstExprPtr node = MakeExpr(EK_BINARY);
		node->op = OP_COMMA;
		node->op_spelling = ",";
		node->operands.push_back(move(left));
		node->operands.push_back(move(right));
		left = move(node);
	}
	return left;
}

// assignment-expression: conditional-expression, optionally followed
// by an assignment-operator and a right-recursive tail.
AstExprPtr AstParser::ParseAssignmentExpression()
{
	// 5.17p1: assignment-expression: throw-expression.
	if (AtSimple(KW_THROW))
	{
		Advance();
		AstExprPtr node = MakeExpr(EK_THROW);
		State state = Save();
		AstExprPtr operand = ParseAssignmentExpression();
		if (operand)
			node->operands.push_back(move(operand));
		else
			Restore(state);
		return node;
	}
	AstExprPtr left = ParseConditionalExpression();
	if (!left)
		return AstExprPtr();
	const ParseToken& token = Peek();
	if (token.kind != PTOK_SIMPLE)
		return left;
	switch (token.simple_type)
	{
	case OP_ASS: case OP_PLUSASS: case OP_MINUSASS: case OP_STARASS:
	case OP_DIVASS: case OP_MODASS: case OP_XORASS: case OP_BANDASS:
	case OP_BORASS: case OP_LSHIFTASS: case OP_RSHIFTASS:
		break;
	default:
		return left;
	}
	State state = Save();
	ETokenType op = token.simple_type;
	string spelling = token.spelling;
	Advance();
	// 5.17p1: the right operand is an initializer-clause; a
	// braced-init-list is accepted directly.
	AstExprPtr right = AtSimple(OP_LBRACE) ? ParseBracedInitList()
	                                       : ParseAssignmentExpression();
	if (!right)
	{
		Restore(state);
		return left;
	}
	AstExprPtr node = MakeExpr(EK_ASSIGNMENT);
	node->op = op;
	node->op_spelling = spelling;
	node->operands.push_back(move(left));
	node->operands.push_back(move(right));
	return node;
}

AstExprPtr AstParser::ParseConditionalExpression()
{
	AstExprPtr condition = ParseBinaryExpression(0);
	if (!condition)
		return AstExprPtr();
	if (!AtSimple(OP_QMARK))
		return condition;
	State state = Save();
	Advance();
	AstExprPtr then_expr = ParseExpression();
	if (!then_expr || !MatchSimple(OP_COLON))
	{
		Restore(state);
		return condition;
	}
	AstExprPtr else_expr = ParseAssignmentExpression();
	if (!else_expr)
	{
		Restore(state);
		return condition;
	}
	AstExprPtr node = MakeExpr(EK_CONDITIONAL);
	node->operands.push_back(move(condition));
	node->operands.push_back(move(then_expr));
	node->operands.push_back(move(else_expr));
	return node;
}

// Binary precedence levels, loosest first; level 10 operands are
// cast-expressions. The relational OP_GT and the split ST_RSHIFT pair
// refuse to act as operators while the innermost bracket is an angle
// (14.2.3); close-angle-bracket owns those tokens there.
bool AstParser::MatchBinaryOperator(int level, ETokenType& op, string& spelling)
{
	const ParseToken& token = Peek();
	if (level == 7)  // shift
	{
		if (token.kind == PTOK_RSHIFT_1 && Peek(1).kind == PTOK_RSHIFT_2 &&
		    !InAngleBrackets())
		{
			Advance();
			Advance();
			op = OP_RSHIFT;
			spelling = ">>";
			return true;
		}
		if (AtSimple(OP_LSHIFT))
		{
			op = OP_LSHIFT;
			spelling = token.spelling;
			Advance();
			return true;
		}
		return false;
	}
	if (token.kind != PTOK_SIMPLE)
		return false;
	ETokenType type = token.simple_type;
	bool matched = false;
	switch (level)
	{
	case 0: matched = type == OP_LOR; break;
	case 1: matched = type == OP_LAND; break;
	case 2: matched = type == OP_BOR; break;
	case 3: matched = type == OP_XOR; break;
	case 4: matched = type == OP_AMP; break;
	case 5: matched = type == OP_EQ || type == OP_NE; break;
	case 6:
		matched = type == OP_LT || type == OP_LE || type == OP_GE ||
			(type == OP_GT && !InAngleBrackets());
		break;
	case 8: matched = type == OP_PLUS || type == OP_MINUS; break;
	case 9:
		matched = type == OP_STAR || type == OP_DIV || type == OP_MOD;
		break;
	case 10: matched = type == OP_DOTSTAR || type == OP_ARROWSTAR; break;
	default: break;
	}
	if (!matched)
		return false;
	op = type;
	spelling = token.spelling;
	Advance();
	return true;
}

AstExprPtr AstParser::ParseBinaryExpression(int level)
{
	if (level > 10)
		return ParseCastExpression();
	AstExprPtr left = ParseBinaryExpression(level + 1);
	if (!left)
		return AstExprPtr();
	for (;;)
	{
		State state = Save();
		ETokenType op = OP_PLUS;
		string spelling;
		if (!MatchBinaryOperator(level, op, spelling))
			break;
		AstExprPtr right = ParseBinaryExpression(level + 1);
		if (!right)
		{
			Restore(state);
			break;
		}
		AstExprPtr node = MakeExpr(EK_BINARY);
		node->op = op;
		node->op_spelling = spelling;
		node->operands.push_back(move(left));
		node->operands.push_back(move(right));
		left = move(node);
	}
	return left;
}

// cast-expression: ( type-id ) cast-expression | unary-expression.
// The type reading is value-blocked through the name table, so
// `(param) << 24` stays a parenthesized expression while
// `(dev_t)(...)` casts.
AstExprPtr AstParser::ParseCastExpression()
{
	if (AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		AstTypeIdPtr type;
		if (ParseTypeId(type) && MatchSimple(OP_RPAREN))
		{
			AstExprPtr operand = ParseCastExpression();
			if (operand)
			{
				AstExprPtr node = MakeExpr(EK_CSTYLE_CAST);
				node->type = move(type);
				node->operands.push_back(move(operand));
				return node;
			}
		}
		Restore(state);
	}
	return ParseUnaryExpression();
}

// sizeof unary-expression | sizeof ( expression ) | sizeof ( type-id ).
// The references resolve the parenthesized form expression-first
// (sizeof(S()) is a call, sizeof(const C*) a type).
AstExprPtr AstParser::ParseSizeofExpression()
{
	State entry = Save();
	Advance();  // KW_SIZEOF
	if (AtSimple(OP_DOTS))
	{
		// sizeof ... ( identifier ): the pack-size operator (5.3.3p5).
		Advance();
		if (MatchSimple(OP_LPAREN) && AtIdentifier())
		{
			string identifier = Peek().spelling;
			Advance();
			if (MatchSimple(OP_RPAREN))
			{
				AstExprPtr node = MakeExpr(EK_SIZEOF_PACK);
				AstNamePart part;
				part.kind = NP_IDENTIFIER;
				part.identifier = identifier;
				node->name.parts.push_back(move(part));
				return node;
			}
		}
		Restore(entry);
		return AstExprPtr();
	}
	if (AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		AstExprPtr expr = ParseExpression();
		if (expr && MatchSimple(OP_RPAREN))
		{
			AstExprPtr node = MakeExpr(EK_SIZEOF_EXPR);
			node->sizeof_paren = true;
			node->operands.push_back(move(expr));
			return node;
		}
		Restore(state);
		Advance();
		AstTypeIdPtr type;
		if (ParseTypeId(type) && MatchSimple(OP_RPAREN))
		{
			AstExprPtr node = MakeExpr(EK_SIZEOF_TYPE);
			node->type = move(type);
			return node;
		}
		Restore(entry);
		return AstExprPtr();
	}
	AstExprPtr operand = ParseUnaryExpression();
	if (!operand)
	{
		Restore(entry);
		return AstExprPtr();
	}
	AstExprPtr node = MakeExpr(EK_SIZEOF_EXPR);
	node->operands.push_back(move(operand));
	return node;
}

// typeid / alignof / noexcept ( ... ), expression-first like sizeof.
AstExprPtr AstParser::ParseTraitExpression(ETokenType keyword)
{
	string spelling = Peek().spelling;
	State entry = Save();
	Advance();
	if (!AtSimple(OP_LPAREN))
	{
		Restore(entry);
		return AstExprPtr();
	}
	State state = Save();
	Advance();
	AstExprPtr node = MakeExpr(EK_TYPE_TRAIT);
	node->op = keyword;
	node->op_spelling = spelling;
	AstExprPtr expr = ParseExpression();
	if (expr && MatchSimple(OP_RPAREN))
	{
		node->operands.push_back(move(expr));
		return node;
	}
	Restore(state);
	Advance();
	AstTypeIdPtr type;
	if (ParseTypeId(type) && MatchSimple(OP_RPAREN))
	{
		node->is_type_operand = true;
		node->type = move(type);
		return node;
	}
	Restore(entry);
	return AstExprPtr();
}

// new-expression; placement is tried first and retried without when
// no type parse follows it (`::new((void*)buf) S(...)` vs `new (x)`).
AstExprPtr AstParser::ParseNewExpression()
{
	State entry = Save();
	AstExprPtr node = MakeExpr(EK_NEW);
	node->global_scope = MatchSimple(OP_COLON2);
	if (!MatchSimple(KW_NEW))
	{
		Restore(entry);
		return AstExprPtr();
	}

	State placement_state = Save();
	if (AtSimple(OP_LPAREN))
	{
		Advance();
		vector<AstExprPtr> args;
		if (ParseInitializerClauseList(args) && !args.empty() &&
		    MatchSimple(OP_RPAREN))
		{
			node->has_placement = true;
			node->arguments.swap(args);
		}
		else
			Restore(placement_state);
	}

	for (int attempt = 0; ; attempt++)
	{
		State type_state = Save();
		if (AtSimple(OP_LPAREN))
		{
			Advance();
			AstTypeIdPtr type;
			if (ParseTypeId(type) && MatchSimple(OP_RPAREN))
			{
				node->paren_type = true;
				node->type = move(type);
				break;
			}
			Restore(type_state);
		}
		// new-type-id: type-specifier-seq with pointer and array
		// declarator parts only.
		AstTypeIdPtr type(new AstTypeId());
		if (ParseSpecifierSeq(type->specifiers, kTypeSpecifierSeq))
		{
			AstDeclaratorPtr declarator(new AstDeclarator());
			ParsePtrOperators(*declarator);
			while (AtSimple(OP_LSQUARE) && !AtSimple(OP_LSQUARE, 1))
			{
				State array_state = Save();
				Advance();
				AstExprPtr bound = ParseExpression();
				if (!bound || !MatchSimple(OP_RSQUARE))
				{
					Restore(array_state);
					break;
				}
				AstDeclaratorItem item;
				item.kind = DI_ARRAY;
				item.array_bound = move(bound);
				declarator->items.push_back(move(item));
			}
			if (!declarator->Empty())
				type->declarator = move(declarator);
			node->type = move(type);
			break;
		}
		Restore(type_state);
		if (attempt == 0 && node->has_placement)
		{
			// The parenthesized list was the type, not a placement.
			Restore(placement_state);
			node->has_placement = false;
			node->arguments.clear();
			continue;
		}
		Restore(entry);
		return AstExprPtr();
	}

	if (AtSimple(OP_LPAREN))
	{
		State init_state = Save();
		Advance();
		AstInitializerPtr init(new AstInitializer());
		init->kind = INIT_PAREN;
		if (!AtSimple(OP_RPAREN))
		{
			if (!ParseInitializerClauseList(init->args))
			{
				Restore(init_state);
				return node;
			}
		}
		if (MatchSimple(OP_RPAREN))
			node->new_init = move(init);
		else
			Restore(init_state);
	}
	else if (AtSimple(OP_LBRACE))
	{
		AstExprPtr braced = ParseBracedInitList();
		if (braced)
		{
			AstInitializerPtr init(new AstInitializer());
			init->kind = INIT_BRACED;
			init->expr = move(braced);
			node->new_init = move(init);
		}
	}
	return node;
}

AstExprPtr AstParser::ParseDeleteExpression()
{
	State entry = Save();
	AstExprPtr node = MakeExpr(EK_DELETE);
	node->global_scope = MatchSimple(OP_COLON2);
	if (!MatchSimple(KW_DELETE))
	{
		Restore(entry);
		return AstExprPtr();
	}
	if (AtSimple(OP_LSQUARE) && AtSimple(OP_RSQUARE, 1))
	{
		Advance();
		Advance();
		node->array_delete = true;
	}
	AstExprPtr operand = ParseCastExpression();
	if (!operand)
	{
		Restore(entry);
		return AstExprPtr();
	}
	node->operands.push_back(move(operand));
	return node;
}

AstExprPtr AstParser::ParseUnaryExpression()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_SIMPLE)
	{
		switch (token.simple_type)
		{
		case OP_INC: case OP_DEC: case OP_STAR: case OP_AMP:
		case OP_PLUS: case OP_MINUS: case OP_LNOT: case OP_COMPL:
		{
			State state = Save();
			ETokenType op = token.simple_type;
			string spelling = token.spelling;
			Advance();
			AstExprPtr operand = ParseCastExpression();
			if (!operand)
			{
				Restore(state);
				return AstExprPtr();
			}
			AstExprPtr node = MakeExpr(EK_UNARY);
			node->op = op;
			node->op_spelling = spelling;
			node->operands.push_back(move(operand));
			return node;
		}
		case KW_SIZEOF:
			return ParseSizeofExpression();
		case KW_ALIGNOF:
		case KW_TYPEID:
		case KW_NOEXCEPT:
			return ParseTraitExpression(token.simple_type);
		case KW_DYNAMIC_CAST:
		case KW_STATIC_CAST:
		case KW_REINTERPET_CAST:
		case KW_CONST_CAST:
		{
			State state = Save();
			AstExprPtr node = MakeExpr(EK_KEYWORD_CAST);
			node->op = token.simple_type;
			node->op_spelling = token.spelling;
			Advance();
			AstTypeIdPtr type;
			AstExprPtr operand;
			if (MatchOpenAngle() && ParseTypeId(type) && MatchCloseAngle() &&
			    MatchSimple(OP_LPAREN) && (operand = ParseExpression()) &&
			    MatchSimple(OP_RPAREN))
			{
				node->type = move(type);
				node->operands.push_back(move(operand));
				// 5.2: the cast is a postfix-expression and accepts
				// further postfix suffixes (`static_cast<T>(x)(y)`).
				return ParsePostfixSuffixes(move(node));
			}
			Restore(state);
			return AstExprPtr();
		}
		case KW_NEW:
			return ParseNewExpression();
		case KW_DELETE:
			return ParseDeleteExpression();
		case OP_COLON2:
			if (AtSimple(KW_NEW, 1))
				return ParseNewExpression();
			if (AtSimple(KW_DELETE, 1))
				return ParseDeleteExpression();
			break;
		default:
			break;
		}
	}
	return ParsePostfixExpression();
}

AstExprPtr AstParser::ParsePostfixExpression()
{
	AstExprPtr expr = ParsePostfixRoot();
	if (!expr)
		return AstExprPtr();
	return ParsePostfixSuffixes(move(expr));
}

AstExprPtr AstParser::ParsePostfixSuffixes(AstExprPtr expr)
{
	for (;;)
	{
		const ParseToken& token = Peek();
		if (AtSimple(OP_LPAREN))
		{
			State state = Save();
			AstExprPtr call = ParseParenExprList(EK_CALL);
			if (!call)
			{
				Restore(state);
				break;
			}
			call->operands.push_back(move(expr));
			expr = move(call);
			continue;
		}
		if (AtSimple(OP_LSQUARE) && !AtSimple(OP_LSQUARE, 1))
		{
			State state = Save();
			Advance();
			AstExprPtr index = ParseExpression();
			if (!index || !MatchSimple(OP_RSQUARE))
			{
				Restore(state);
				break;
			}
			AstExprPtr node = MakeExpr(EK_SUBSCRIPT);
			node->operands.push_back(move(expr));
			node->operands.push_back(move(index));
			expr = move(node);
			continue;
		}
		if (AtSimple(OP_DOT) || AtSimple(OP_ARROW))
		{
			State state = Save();
			ETokenType op = token.simple_type;
			string spelling = token.spelling;
			Advance();
			// PA21 14.2p4: `. template` / `-> template` marks the
			// member name as a template-id; the terminal identifier
			// gains the template classification for the clause parse.
			bool template_keyword = false;
			if (AtSimple(KW_TEMPLATE) && AtIdentifier(1))
			{
				template_keyword = true;
				Advance();
				Register(Peek().spelling, NF_VALUE | NF_TEMPLATE);
			}
			AstName name;
			if (!ParseIdExpressionName(name))
			{
				Restore(state);
				break;
			}
			if (template_keyword)
				name.parts.front().template_keyword = true;
			AstExprPtr node = MakeExpr(EK_MEMBER);
			node->op = op;
			node->op_spelling = spelling;
			node->operands.push_back(move(expr));
			node->name = move(name);
			expr = move(node);
			continue;
		}
		if (AtSimple(OP_INC) || AtSimple(OP_DEC))
		{
			AstExprPtr node = MakeExpr(EK_POSTFIX_INCDEC);
			node->op = token.simple_type;
			node->op_spelling = token.spelling;
			Advance();
			node->operands.push_back(move(expr));
			expr = move(node);
			continue;
		}
		break;
	}
	return expr;
}

// postfix roots: a simple-type keyword sequence followed by `(` is a
// function-style cast (dumped with a paren-argument-list); everything
// else is a primary-expression. The sequence form (`unsigned long(e)`)
// keeps every keyword in `cast_keywords` for the semantic passes.
// PA34 builtin trait: __is_*/__has_* ( type-id-list ), evaluated by
// the sema to a bool constant. Argument type-ids may be pack-expanded
// (`Args...`). Returns null (state restored) when the operand list
// does not parse, so the name can fall back to an id-expression.
AstExprPtr AstParser::ParseBuiltinTraitExpression()
{
	State entry = Save();
	AstExprPtr node = MakeExpr(EK_BUILTIN_TRAIT);
	node->op_spelling = Peek().spelling;
	Advance();
	Advance();  // the ( after the trait name
	for (;;)
	{
		AstTemplateArgument argument;
		argument.is_type = true;
		if (!ParseTypeId(argument.type))
		{
			Restore(entry);
			return AstExprPtr();
		}
		if (AtSimple(OP_DOTS))
		{
			argument.pack = true;
			Advance();
		}
		node->trait_args.push_back(move(argument));
		if (!MatchSimple(OP_COMMA))
			break;
	}
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(entry);
		return AstExprPtr();
	}
	return node;
}

AstExprPtr AstParser::ParsePostfixRoot()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_IDENTIFIER &&
	    HostedBuiltinTraitName(token.spelling) && AtSimple(OP_LPAREN, 1))
	{
		AstExprPtr trait = ParseBuiltinTraitExpression();
		if (trait)
			return trait;
	}
	if (token.kind == PTOK_SIMPLE && IsSimpleTypeKeyword(token.simple_type))
	{
		size_t keywords = 1;
		while (Peek(keywords).kind == PTOK_SIMPLE &&
		       IsSimpleTypeKeyword(Peek(keywords).simple_type))
			keywords++;
		if (AtSimple(OP_LPAREN, keywords) || AtSimple(OP_LBRACE, keywords))
		{
			State entry = Save();
			bool braced_form = AtSimple(OP_LBRACE, keywords);
			AstExprPtr node = MakeExpr(EK_FUNCTIONAL_CAST);
			node->op = token.simple_type;
			for (size_t i = 0; i < keywords; i++)
			{
				const ParseToken& keyword = Peek();
				node->cast_keywords.push_back(keyword.simple_type);
				if (i)
					node->op_spelling += " ";
				node->op_spelling += keyword.spelling;
				Advance();
			}
			if (braced_form)
			{
				// 5.2.3p3: simple-type-specifier braced-init-list.
				AstExprPtr braced = ParseBracedInitList();
				if (!braced)
				{
					Restore(entry);
					return AstExprPtr();
				}
				node->arguments.swap(braced->arguments);
				return node;
			}
			AstExprPtr args = ParseParenExprList(EK_FUNCTIONAL_CAST);
			if (!args)
			{
				Restore(entry);
				return AstExprPtr();
			}
			node->arguments.swap(args->arguments);
			return node;
		}
	}
	if (AtSimple(KW_TYPENAME))
	{
		// PA21 14.6p2: `typename T::X(args)` functional-casts the
		// named dependent type; the qualified name parses as the
		// id-expression root and the callee re-reads as a type.
		State entry = Save();
		AstName name;
		if (ParseQualifiedTypeName(name) &&
		    (AtSimple(OP_LPAREN) || AtSimple(OP_LBRACE)))
		{
			AstExprPtr node = MakeExpr(EK_ID);
			node->name = move(name);
			return node;
		}
		Restore(entry);
	}
	if (AtSimple(KW_DECLTYPE) && AtSimple(OP_LPAREN, 1))
	{
		// decltype-specifier as a postfix root (`decltype(x)(1)`): an
		// id-expression with one decltype part; the suffix loop attaches
		// the cast's argument list as a call. A trailing `::` is a
		// decltype-rooted qualified-id instead (primary-expression).
		State entry = Save();
		Advance();
		Advance();
		AstExprPtr inner = ParseExpression();
		if (inner && MatchSimple(OP_RPAREN) && !AtSimple(OP_COLON2))
		{
			AstExprPtr node = MakeExpr(EK_ID);
			AstNamePart part;
			part.kind = NP_DECLTYPE;
			part.decltype_expr = move(inner);
			node->name.parts.push_back(move(part));
			return node;
		}
		Restore(entry);
	}
	AstExprPtr expr = ParsePrimaryExpression();
	// 5.2.3p3: simple-type-specifier braced-init-list builds a temporary
	// (`T{...}`). Only an id-expression can name the type here, and a
	// braced-init-list never follows a value-id inside an expression, so
	// the call shape reuses the function-style cast path with the braced
	// items as its arguments.
	if (expr && expr->kind == EK_ID && AtSimple(OP_LBRACE))
	{
		AstExprPtr braced = ParseBracedInitList();
		if (braced)
		{
			AstExprPtr call = MakeExpr(EK_CALL);
			call->arguments.swap(braced->arguments);
			call->operands.push_back(move(expr));
			return call;
		}
	}
	return expr;
}

AstExprPtr AstParser::ParsePrimaryExpression()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_LITERAL)
	{
		AstExprPtr node = MakeExpr(EK_LITERAL);
		node->literal = token.spelling;
		node->literal_kind = token.literal_kind;
		node->literal_type = token.literal_type;
		node->literal_elements = token.literal_elements;
		node->literal_data = token.literal_data;
		node->literal_suffix = token.ud_suffix;
		Advance();
		return node;
	}
	if (token.kind == PTOK_SIMPLE)
	{
		switch (token.simple_type)
		{
		case KW_TRUE: case KW_FALSE: case KW_NULLPTR: case KW_THIS:
		{
			AstExprPtr node = MakeExpr(EK_KEYWORD_LITERAL);
			node->op = token.simple_type;
			node->literal = token.spelling;
			Advance();
			return node;
		}
		case OP_LPAREN:
		{
			State state = Save();
			Advance();
			// GNU statement expression: ( compound-statement ).
			if (AtSimple(OP_LBRACE))
			{
				AstStmtPtr body = ParseCompoundStatement();
				if (body && MatchSimple(OP_RPAREN))
				{
					AstExprPtr node = MakeExpr(EK_STATEMENT_EXPR);
					node->stmt_body = move(body);
					return node;
				}
				Restore(state);
				return AstExprPtr();
			}
			AstExprPtr inner = ParseExpression();
			if (!inner || !MatchSimple(OP_RPAREN))
			{
				Restore(state);
				return AstExprPtr();
			}
			AstExprPtr node = MakeExpr(EK_PAREN);
			node->operands.push_back(move(inner));
			return node;
		}
		case OP_LSQUARE:
			return ParseLambdaExpression();
		default:
			break;
		}
	}
	// __builtin_va_arg ( assignment-expression , type-id ): the SysV
	// vararg fetch takes a type operand, so it cannot parse as an
	// ordinary call.
	if (AtIdentifierSpelled("__builtin_va_arg"))
	{
		State state = Save();
		Advance();
		if (MatchSimple(OP_LPAREN))
		{
			AstExprPtr list = ParseAssignmentExpression();
			AstTypeIdPtr type;
			if (list && MatchSimple(OP_COMMA) && ParseTypeId(type) &&
			    MatchSimple(OP_RPAREN))
			{
				AstExprPtr node = MakeExpr(EK_VA_ARG);
				node->operands.push_back(move(list));
				node->type = move(type);
				return node;
			}
		}
		Restore(state);
	}
	AstName name;
	if (!ParseIdExpressionName(name))
		return AstExprPtr();
	AstExprPtr node = MakeExpr(EK_ID);
	node->name = move(name);
	return node;
}

// lambda-introducer: [ lambda-capture? ] with lambda-capture =
// capture-default (, capture-list)? | capture-list. A lone `&` (not
// followed by an identifier) is the by-reference capture-default;
// `&x` is a capture.
bool AstParser::ParseLambdaIntroducer(AstLambda& lambda)
{
	if (!MatchSimple(OP_LSQUARE))
		return false;
	if (MatchSimple(OP_RSQUARE))
		return true;
	if (AtSimple(OP_ASS) || (AtSimple(OP_AMP) && !AtIdentifier(1)))
	{
		lambda.has_capture_default = true;
		lambda.capture_default = Peek().simple_type;
		Advance();
		if (MatchSimple(OP_RSQUARE))
			return true;
		if (!MatchSimple(OP_COMMA))
			return false;
	}
	for (;;)
	{
		AstLambdaCapture capture;
		if (MatchSimple(KW_THIS))
			capture.kind = LC_THIS;
		else if (MatchSimple(OP_AMP))
		{
			if (!AtIdentifier())
				return false;
			capture.kind = LC_REF;
			capture.identifier = Peek().spelling;
			Advance();
		}
		else if (AtIdentifier())
		{
			capture.kind = LC_COPY;
			capture.identifier = Peek().spelling;
			Advance();
		}
		else
			return false;
		capture.pack = MatchSimple(OP_DOTS);
		lambda.captures.push_back(move(capture));
		if (!MatchSimple(OP_COMMA))
			break;
	}
	return MatchSimple(OP_RSQUARE);
}

AstExprPtr AstParser::ParseLambdaExpression()
{
	State state = Save();
	AstLambdaPtr lambda(new AstLambda());
	if (!ParseLambdaIntroducer(*lambda))
	{
		Restore(state);
		return AstExprPtr();
	}
	lambda->declarator_begin_token = Save().pos;
	if (AtSimple(OP_LPAREN))
	{
		if (!ParseParameterClause(lambda->parameters))
		{
			Restore(state);
			return AstExprPtr();
		}
		lambda->has_declarator = true;
		if (AtSimple(KW_MUTABLE))
		{
			lambda->mutable_specifier = true;
			lambda->mutable_spelling = Peek().spelling;
			Advance();
		}
		if (AtSimple(KW_NOEXCEPT))
		{
			Advance();
			lambda->has_noexcept = true;
			if (AtSimple(OP_LPAREN))
			{
				State noexcept_state = Save();
				Advance();
				AstExprPtr expr = ParseExpression();
				if (expr && MatchSimple(OP_RPAREN))
					lambda->noexcept_expr = move(expr);
				else
					Restore(noexcept_state);
			}
		}
		if (MatchSimple(OP_ARROW))
		{
			AstTypeIdPtr type;
			if (!ParseTypeId(type))
			{
				Restore(state);
				return AstExprPtr();
			}
			lambda->has_trailing = true;
			lambda->trailing_type = move(type);
		}
	}
	lambda->body_begin_token = Save().pos;
	PushTransientScope();
	if (lambda->parameters)
	{
		AstDeclarator dummy;
		AstDeclaratorItem item;
		item.kind = DI_PARAMS;
		item.params = move(lambda->parameters);
		dummy.items.push_back(move(item));
		RegisterParameters(dummy);
		lambda->parameters = move(dummy.items[0].params);
	}
	AstStmtPtr body = ParseCompoundStatement();
	PopScope();
	if (!body)
	{
		Restore(state);
		return AstExprPtr();
	}
	lambda->body = move(body);
	AstExprPtr node = MakeExpr(EK_LAMBDA);
	node->lambda = move(lambda);
	return node;
}

AstExprPtr AstParser::ParseInitializerClause()
{
	if (AtSimple(OP_LBRACE))
		return ParseBracedInitList();
	return ParseAssignmentExpression();
}

// initializer-clause (OP_DOTS)? (, ...)* — pack expansions wrap their
// clause.
bool AstParser::ParseInitializerClauseList(vector<AstExprPtr>& list)
{
	AstExprPtr first = ParseInitializerClause();
	if (!first)
		return false;
	if (MatchSimple(OP_DOTS))
	{
		AstExprPtr pack = MakeExpr(EK_PACK_EXPANSION);
		pack->operands.push_back(move(first));
		first = move(pack);
	}
	list.push_back(move(first));
	for (;;)
	{
		State state = Save();
		if (!MatchSimple(OP_COMMA))
			return true;
		AstExprPtr next = ParseInitializerClause();
		if (!next)
		{
			Restore(state);
			return true;
		}
		if (MatchSimple(OP_DOTS))
		{
			AstExprPtr pack = MakeExpr(EK_PACK_EXPANSION);
			pack->operands.push_back(move(next));
			next = move(pack);
		}
		list.push_back(move(next));
	}
}

AstExprPtr AstParser::ParseBracedInitList()
{
	State state = Save();
	if (!MatchSimple(OP_LBRACE))
		return AstExprPtr();
	AstExprPtr node = MakeExpr(EK_BRACED);
	if (!AtSimple(OP_RBRACE))
	{
		if (!ParseInitializerClauseList(node->arguments))
		{
			Restore(state);
			return AstExprPtr();
		}
		MatchSimple(OP_COMMA);  // trailing comma
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return AstExprPtr();
	}
	return node;
}

// ( initializer-clause-list? ) for call arguments and function-style
// casts.
AstExprPtr AstParser::ParseParenExprList(EExprKind kind)
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return AstExprPtr();
	AstExprPtr node = MakeExpr(kind);
	if (!AtSimple(OP_RPAREN))
	{
		if (!ParseInitializerClauseList(node->arguments))
		{
			Restore(state);
			return AstExprPtr();
		}
	}
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return AstExprPtr();
	}
	return node;
}
