// Expressions: primary/lambda/postfix/unary forms, new and delete,
// casts, the table-driven binary-operator ladder, conditional and
// assignment expressions, and the initializer-clause family.

#include "parser.h"

using std::move;

namespace {

// The pm-expression .. logical-or-expression ladder, outermost first.
// Each level is `lower (op lower)*`; shift and relational get special
// handling for the split `>>` and the 14.2.3 angle refusals.
struct BinaryLevel
{
	const char* name;
	ETokenType ops[4];
	int num_ops;
};

const BinaryLevel kBinaryLevels[] = {
	{ "logical-or-expression", { OP_LOR }, 1 },
	{ "logical-and-expression", { OP_LAND }, 1 },
	{ "inclusive-or-expression", { OP_BOR }, 1 },
	{ "exclusive-or-expression", { OP_XOR }, 1 },
	{ "and-expression", { OP_AMP }, 1 },
	{ "equality-expression", { OP_EQ, OP_NE }, 2 },
	{ "relational-expression", { OP_LT, OP_GT, OP_LE, OP_GE }, 4 },
	{ "shift-expression", { OP_LSHIFT }, 1 },
	{ "additive-expression", { OP_PLUS, OP_MINUS }, 2 },
	{ "multiplicative-expression", { OP_STAR, OP_DIV, OP_MOD }, 3 },
	{ "pm-expression", { OP_DOTSTAR, OP_ARROWSTAR }, 2 },
};

const int kNumBinaryLevels =
	sizeof(kBinaryLevels) / sizeof(kBinaryLevels[0]);
const int kShiftLevel = 7;

} // namespace

ParseNodePtr Parser::ParseBinaryExpression(int level)
{
	if (level >= kNumBinaryLevels)
		return ParseCastExpression();
	ParseNodePtr lhs = ParseBinaryExpression(level + 1);
	if (!lhs)
		return ParseNodePtr();
	for (;;)
	{
		State state = Save();
		ParseNodePtr op = ParseBinaryOperator(level);
		if (!op)
			break;
		ParseNodePtr rhs = ParseBinaryExpression(level + 1);
		if (!rhs)
		{
			Restore(state);
			break;
		}
		ParseNodePtr node = MakeParseNode(kBinaryLevels[level].name);
		node->Add(move(lhs));
		node->Add(move(op));
		node->Add(move(rhs));
		lhs = move(node);
	}
	return lhs;
}

// One operator of the given ladder level. The relational OP_GT and the
// shift ST_RSHIFT_1 ST_RSHIFT_2 pair refuse to match while the
// innermost bracket is an angle: those tokens are reserved for
// close-angle-bracket at the same nesting level (14.2.3).
ParseNodePtr Parser::ParseBinaryOperator(int level)
{
	if (level == kShiftLevel && Peek().kind == PTOK_RSHIFT_1 &&
	    Peek(1).kind == PTOK_RSHIFT_2 && !InAngleBrackets())
	{
		ParseNodePtr op = MakeParseNode("shift-operator");
		op->Add(MakeTokenLeaf(Peek()));
		Advance();
		op->Add(MakeTokenLeaf(Peek()));
		Advance();
		return op;
	}
	const BinaryLevel& spec = kBinaryLevels[level];
	for (int i = 0; i < spec.num_ops; i++)
	{
		if (spec.ops[i] == OP_GT && InAngleBrackets())
			continue;
		// 14.2/3: a < following a name that lookup (here: the mock rules)
		// finds to be a template-name is always the template-argument-list
		// delimiter, never the less-than operator. simple-template-id
		// already had its greedy chance at the name; the relational
		// reading must not reinterpret the <.
		if (spec.ops[i] == OP_LT && pos_ > 0 &&
		    tokens_[pos_ - 1].kind == PTOK_IDENTIFIER &&
		    tokens_[pos_ - 1].HasFlag(PTF_TEMPLATE_NAME))
			continue;
		if (AtSimple(spec.ops[i]))
			return MatchSimpleLeaf(spec.ops[i]);
	}
	return ParseNodePtr();
}

// The (OP_QMARK expression OP_COLON assignment-expression)? suffix;
// passes the condition through unchanged when no complete suffix
// follows.
ParseNodePtr Parser::ParseConditionalSuffix(ParseNodePtr condition)
{
	State state = Save();
	if (MatchSimple(OP_QMARK))
	{
		ParseNodePtr on_true = ParseExpression();
		ParseNodePtr on_false;
		if (on_true && MatchSimple(OP_COLON) &&
		    (on_false = ParseAssignmentExpression()))
		{
			ParseNodePtr node = MakeParseNode("conditional-expression");
			node->Add(move(condition));
			node->Add(move(on_true));
			node->Add(move(on_false));
			return node;
		}
		Restore(state);
	}
	return condition;
}

ParseNodePtr Parser::ParseConditionalExpression()
{
	return MemoParse(kMemoConditionalExpression,
	                 &Parser::ParseConditionalExpressionRule);
}

ParseNodePtr Parser::ParseConditionalExpressionRule()
{
	ParseNodePtr lhs = ParseBinaryExpression(0);
	if (!lhs)
		return ParseNodePtr();
	return ParseConditionalSuffix(move(lhs));
}

// assignment-expression: conditional-expression |
//     logical-or-expression assignment-operator initializer-clause |
//     throw-expression
// Factored: the ladder is parsed once and extended by `? :` or an
// assignment operator, exactly because the assignment lhs is the
// ladder (logical-or) parse.
ParseNodePtr Parser::ParseAssignmentExpression()
{
	return MemoParse(kMemoAssignmentExpression,
	                 &Parser::ParseAssignmentExpressionRule);
}

ParseNodePtr Parser::ParseAssignmentExpressionRule()
{
	if (AtSimple(KW_THROW))
		return ParseThrowExpression();
	ParseNodePtr lhs = ParseBinaryExpression(0);
	if (!lhs)
		return ParseNodePtr();
	if (AtSimple(OP_QMARK))
		return ParseConditionalSuffix(move(lhs));
	static const ETokenType kAssignmentOps[] = {
		OP_ASS, OP_STARASS, OP_DIVASS, OP_MODASS, OP_PLUSASS,
		OP_MINUSASS, OP_RSHIFTASS, OP_LSHIFTASS, OP_BANDASS, OP_XORASS,
		OP_BORASS
	};
	for (size_t i = 0; i < sizeof(kAssignmentOps) / sizeof(ETokenType); i++)
	{
		if (!AtSimple(kAssignmentOps[i]))
			continue;
		State state = Save();
		ParseNodePtr op = MatchSimpleLeaf(kAssignmentOps[i]);
		ParseNodePtr rhs = ParseInitializerClause();
		if (!rhs)
		{
			Restore(state);
			break;
		}
		ParseNodePtr node = MakeParseNode("assignment-expression");
		node->Add(move(lhs));
		node->Add(move(op));
		node->Add(move(rhs));
		return node;
	}
	return lhs;
}

// expression: assignment-expression (OP_COMMA assignment-expression)*
ParseNodePtr Parser::ParseExpression()
{
	return MemoParse(kMemoExpression, &Parser::ParseExpressionRule);
}

ParseNodePtr Parser::ParseExpressionRule()
{
	return ParseCommaList("expression", &Parser::ParseAssignmentExpression);
}

// constant-expression: conditional-expression
ParseNodePtr Parser::ParseConstantExpression()
{
	return ParseConditionalExpression();
}

// throw-expression: KW_THROW assignment-expression?
ParseNodePtr Parser::ParseThrowExpression()
{
	ParseNodePtr kw = MatchSimpleLeaf(KW_THROW);
	if (!kw)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("throw-expression");
	node->Add(move(kw));
	ParseNodePtr operand = ParseAssignmentExpression();
	if (operand)
		node->Add(move(operand));
	return node;
}

// primary-expression: KW_TRUE | KW_FALSE | KW_NULLPTR | TT_LITERAL |
//     KW_THIS | OP_LPAREN expression OP_RPAREN | id-expression |
//     lambda-expression
ParseNodePtr Parser::ParsePrimaryExpression()
{
	ParseNodePtr node = MakeParseNode("primary-expression");
	if (AtSimple(KW_TRUE) || AtSimple(KW_FALSE) || AtSimple(KW_NULLPTR) ||
	    AtSimple(KW_THIS))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		return node;
	}
	if (AtLiteral())
	{
		node->Add(MatchLiteralLeaf());
		return node;
	}
	if (AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		ParseNodePtr expression = ParseExpression();
		if (expression && MatchSimple(OP_RPAREN))
		{
			node->Add(move(expression));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(OP_LSQUARE))
	{
		ParseNodePtr lambda = ParseLambdaExpression();
		if (!lambda)
			return ParseNodePtr();
		node->Add(move(lambda));
		return node;
	}
	ParseNodePtr id = ParseIdExpression();
	if (!id)
		return ParseNodePtr();
	node->Add(move(id));
	return node;
}

// lambda-expression: lambda-introducer lambda-declarator?
//     compound-statement
ParseNodePtr Parser::ParseLambdaExpression()
{
	State state = Save();
	ParseNodePtr introducer = ParseLambdaIntroducer();
	if (!introducer)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("lambda-expression");
	node->Add(move(introducer));
	ParseNodePtr declarator = ParseLambdaDeclarator();
	if (declarator)
		node->Add(move(declarator));
	ParseNodePtr body = ParseCompoundStatement();
	if (!body)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(body));
	return node;
}

// lambda-introducer: OP_LSQUARE lambda-capture? OP_RSQUARE
ParseNodePtr Parser::ParseLambdaIntroducer()
{
	State state = Save();
	if (!MatchSimple(OP_LSQUARE))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("lambda-introducer");
	if (!AtSimple(OP_RSQUARE))
	{
		ParseNodePtr capture = ParseLambdaCapture();
		if (!capture)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(capture));
	}
	if (!MatchSimple(OP_RSQUARE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// lambda-capture: capture-default | capture-list |
//     capture-default OP_COMMA capture-list
ParseNodePtr Parser::ParseLambdaCapture()
{
	if (AtSimple(OP_AMP) || AtSimple(OP_ASS))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("lambda-capture");
		ParseNodePtr capture_default = MakeParseNode("capture-default");
		capture_default->Add(MakeTokenLeaf(Peek()));
		Advance();
		node->Add(move(capture_default));
		if (AtSimple(OP_RSQUARE))
			return node;
		if (MatchSimple(OP_COMMA))
		{
			ParseNodePtr list = ParseCaptureList();
			if (list)
			{
				node->Add(move(list));
				return node;
			}
		}
		Restore(state);
	}
	ParseNodePtr list = ParseCaptureList();
	if (!list)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("lambda-capture");
	node->Add(move(list));
	return node;
}

// capture-list: capture OP_DOTS? (OP_COMMA capture OP_DOTS?)*
ParseNodePtr Parser::ParseCaptureList()
{
	return ParseCommaList("capture-list", &Parser::ParseCaptureDots);
}

ParseNodePtr Parser::ParseCaptureDots()
{
	return ParseDotsItem("capture-dots", &Parser::ParseCapture);
}

// capture: TT_IDENTIFIER | OP_AMP TT_IDENTIFIER | KW_THIS
ParseNodePtr Parser::ParseCapture()
{
	ParseNodePtr node = MakeParseNode("capture");
	if (AtSimple(KW_THIS))
	{
		node->Add(MatchSimpleLeaf(KW_THIS));
		return node;
	}
	if (AtSimple(OP_AMP) && AtIdentifier(1))
	{
		node->Add(MatchSimpleLeaf(OP_AMP));
		node->Add(MatchIdentifierLeaf());
		return node;
	}
	if (AtIdentifier())
	{
		node->Add(MatchIdentifierLeaf());
		return node;
	}
	return ParseNodePtr();
}

// lambda-declarator: OP_LPAREN parameter-declaration-clause OP_RPAREN
//     KW_MUTABLE? exception-specification? attribute-specifier*
//     trailing-return-type?
ParseNodePtr Parser::ParseLambdaDeclarator()
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("lambda-declarator");
	ParseNodePtr parameters = ParseParameterDeclarationClause();
	if (!parameters || !MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(parameters));
	if (AtSimple(KW_MUTABLE))
		node->Add(MatchSimpleLeaf(KW_MUTABLE));
	ParseNodePtr exception = ParseExceptionSpecification();
	if (exception)
		node->Add(move(exception));
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr trailing = ParseTrailingReturnType();
	if (trailing)
		node->Add(move(trailing));
	return node;
}

// postfix-expression: postfix-root postfix-suffix*
ParseNodePtr Parser::ParsePostfixExpression()
{
	ParseNodePtr lhs = ParsePostfixRoot();
	if (!lhs)
		return ParseNodePtr();
	for (;;)
	{
		ParseNodePtr suffix = ParsePostfixSuffix();
		if (!suffix)
			break;
		ParseNodePtr node = MakeParseNode("postfix-expression");
		node->Add(move(lhs));
		node->Add(move(suffix));
		lhs = move(node);
	}
	return lhs;
}

// postfix-root: primary-expression | functional casts from
//     simple-type-specifier/typename-specifier | the four *_cast
//     keywords | KW_TYPEID forms. The type-led functional-cast forms
//     are tried before primary-expression so a type-name followed by
//     ( or { takes the cast reading (8.2).
ParseNodePtr Parser::ParsePostfixRoot()
{
	if (AtSimple(KW_DYNAMIC_CAST) || AtSimple(KW_STATIC_CAST) ||
	    AtSimple(KW_REINTERPET_CAST) || AtSimple(KW_CONST_CAST))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("postfix-root");
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		ParseNodePtr type;
		ParseNodePtr close;
		ParseNodePtr expression;
		if (MatchOpenAngle() && (type = ParseTypeId()) &&
		    (close = ParseCloseAngleBracket()) && MatchSimple(OP_LPAREN) &&
		    (expression = ParseExpression()) && MatchSimple(OP_RPAREN))
		{
			node->Add(move(type));
			node->Add(move(close));
			node->Add(move(expression));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_TYPEID))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("postfix-root");
		node->Add(MatchSimpleLeaf(KW_TYPEID));
		if (!MatchSimple(OP_LPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		State operand_state = Save();
		ParseNodePtr operand = ParseTypeId();
		if (operand && MatchSimple(OP_RPAREN))
		{
			node->Add(move(operand));
			return node;
		}
		Restore(operand_state);
		operand = ParseExpression();
		if (operand && MatchSimple(OP_RPAREN))
		{
			node->Add(move(operand));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	State state = Save();
	ParseNodePtr type;
	if (AtSimple(KW_TYPENAME))
		type = ParseTypenameSpecifier();
	else
		type = ParseSimpleTypeSpecifier();
	if (type)
	{
		ParseNodePtr node = MakeParseNode("postfix-root");
		if (AtSimple(OP_LPAREN))
		{
			Advance();
			ParseNodePtr arguments;
			if (!AtSimple(OP_RPAREN))
				arguments = ParseExpressionList();
			bool bad_arguments = !AtSimple(OP_RPAREN);
			if (!bad_arguments && MatchSimple(OP_RPAREN))
			{
				node->Add(move(type));
				if (arguments)
					node->Add(move(arguments));
				return node;
			}
		}
		else if (AtSimple(OP_LBRACE))
		{
			ParseNodePtr braced = ParseBracedInitList();
			if (braced)
			{
				node->Add(move(type));
				node->Add(move(braced));
				return node;
			}
		}
		Restore(state);
	}
	return ParsePrimaryExpression();
}

// postfix-suffix: subscript, call, member access (with
//     pseudo-destructor-names), or ++/--
ParseNodePtr Parser::ParsePostfixSuffix()
{
	ParseNodePtr node = MakeParseNode("postfix-suffix");
	if (AtSimple(OP_LSQUARE))
	{
		State state = Save();
		Advance();
		ParseNodePtr index;
		if (AtSimple(OP_LBRACE))
			index = ParseBracedInitList();
		else
			index = ParseExpression();
		if (index && MatchSimple(OP_RSQUARE))
		{
			node->Add(move(index));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(OP_LPAREN))
	{
		State state = Save();
		Advance();
		ParseNodePtr arguments;
		if (!AtSimple(OP_RPAREN))
		{
			arguments = ParseExpressionList();
			if (!arguments)
			{
				Restore(state);
				return ParseNodePtr();
			}
		}
		if (!MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		ParseNodePtr call = MakeParseNode("call-arguments");
		if (arguments)
			call->Add(move(arguments));
		node->Add(move(call));
		return node;
	}
	if (AtSimple(OP_DOT) || AtSimple(OP_ARROW))
	{
		State state = Save();
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		ParseNodePtr member = ParsePseudoDestructorName();
		if (member)
		{
			node->Add(move(member));
			return node;
		}
		if (AtSimple(KW_TEMPLATE))
			node->Add(MatchSimpleLeaf(KW_TEMPLATE));
		member = ParseIdExpression();
		if (member)
		{
			node->Add(move(member));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(OP_INC) || AtSimple(OP_DEC))
	{
		node->Add(MakeTokenLeaf(Peek()));
		Advance();
		return node;
	}
	return ParseNodePtr();
}

// expression-list: initializer-list
ParseNodePtr Parser::ParseExpressionList()
{
	ParseNodePtr list = ParseInitializerList();
	if (!list)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("expression-list");
	node->Add(move(list));
	return node;
}

// pseudo-destructor-name: nested-name-specifier? OP_COMPL type-name |
//     OP_COMPL decltype-specifier
ParseNodePtr Parser::ParsePseudoDestructorName()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("pseudo-destructor-name");
	if (AtSimple(OP_COMPL) && AtSimple(KW_DECLTYPE, 1))
	{
		node->Add(MatchSimpleLeaf(OP_COMPL));
		ParseNodePtr decltype_spec = ParseDecltypeSpecifier();
		if (decltype_spec)
		{
			node->Add(move(decltype_spec));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr compl_leaf = MatchSimpleLeaf(OP_COMPL);
	ParseNodePtr name;
	if (!compl_leaf || !(name = ParseTypeName()))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(compl_leaf));
	node->Add(move(name));
	return node;
}

// unary-expression: postfix-expression | unary-operator
//     cast-expression | sizeof/alignof forms | noexcept-expression |
//     new-expression | delete-expression
ParseNodePtr Parser::ParseUnaryExpression()
{
	return MemoParse(kMemoUnaryExpression,
	                 &Parser::ParseUnaryExpressionRule);
}

ParseNodePtr Parser::ParseUnaryExpressionRule()
{
	if (AtSimple(KW_SIZEOF))
		return ParseSizeofExpression();
	if (AtSimple(KW_ALIGNOF))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("unary-expression");
		node->Add(MatchSimpleLeaf(KW_ALIGNOF));
		ParseNodePtr type;
		if (MatchSimple(OP_LPAREN) && (type = ParseTypeId()) &&
		    MatchSimple(OP_RPAREN))
		{
			node->Add(move(type));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_NOEXCEPT))
	{
		State state = Save();
		ParseNodePtr node = MakeParseNode("noexcept-expression");
		node->Add(MatchSimpleLeaf(KW_NOEXCEPT));
		ParseNodePtr expression;
		if (MatchSimple(OP_LPAREN) && (expression = ParseExpression()) &&
		    MatchSimple(OP_RPAREN))
		{
			node->Add(move(expression));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	if (AtSimple(KW_NEW) || (AtSimple(OP_COLON2) && AtSimple(KW_NEW, 1)))
		return ParseNewExpression();
	if (AtSimple(KW_DELETE) ||
	    (AtSimple(OP_COLON2) && AtSimple(KW_DELETE, 1)))
		return ParseDeleteExpression();
	if (AtSimple(OP_COMPL))
	{
		// grammar order: a postfix-expression (`~C()` destructor-style
		// call) wins over OP_COMPL cast-expression
		ParseNodePtr postfix = ParsePostfixExpression();
		if (postfix)
			return postfix;
	}
	static const ETokenType kUnaryOps[] = {
		OP_INC, OP_DEC, OP_STAR, OP_AMP, OP_PLUS, OP_MINUS, OP_LNOT,
		OP_COMPL
	};
	for (size_t i = 0; i < sizeof(kUnaryOps) / sizeof(ETokenType); i++)
	{
		if (!AtSimple(kUnaryOps[i]))
			continue;
		State state = Save();
		ParseNodePtr node = MakeParseNode("unary-expression");
		node->Add(MatchSimpleLeaf(kUnaryOps[i]));
		ParseNodePtr operand = ParseCastExpression();
		if (!operand)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(operand));
		return node;
	}
	return ParsePostfixExpression();
}

// KW_SIZEOF unary-expression | KW_SIZEOF OP_LPAREN type-id OP_RPAREN |
// KW_SIZEOF OP_DOTS OP_LPAREN TT_IDENTIFIER OP_RPAREN
// The expression reading is tried before the parenthesized type-id:
// it can consume postfix suffixes beyond the parenthesized group
// (`sizeof(C1)(x)` is a call of a parenthesized id-expression, which
// the grammar reduces and the reference accepts), while anything only
// readable as a type still falls through to the type-id form.
ParseNodePtr Parser::ParseSizeofExpression()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("unary-expression");
	ParseNodePtr kw = MatchSimpleLeaf(KW_SIZEOF);
	if (!kw)
		return ParseNodePtr();
	node->Add(move(kw));
	if (AtSimple(OP_DOTS))
	{
		node->Add(MatchSimpleLeaf(OP_DOTS));
		ParseNodePtr name;
		if (MatchSimple(OP_LPAREN) && (name = MatchIdentifierLeaf()) &&
		    MatchSimple(OP_RPAREN))
		{
			node->Add(move(name));
			return node;
		}
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr operand = ParseUnaryExpression();
	if (operand)
	{
		node->Add(move(operand));
		return node;
	}
	State operand_state = Save();
	if (MatchSimple(OP_LPAREN))
	{
		ParseNodePtr type = ParseTypeId();
		if (type && MatchSimple(OP_RPAREN))
		{
			node->Add(move(type));
			return node;
		}
		Restore(operand_state);
	}
	Restore(state);
	return ParseNodePtr();
}

// new-expression: OP_COLON2? KW_NEW new-placement? new-type-id
//     new-initializer? | OP_COLON2? KW_NEW new-placement?
//     OP_LPAREN type-id OP_RPAREN new-initializer?
ParseNodePtr Parser::ParseNewExpression()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("new-expression");
	if (AtSimple(OP_COLON2))
		node->Add(MatchSimpleLeaf(OP_COLON2));
	ParseNodePtr kw = MatchSimpleLeaf(KW_NEW);
	if (!kw)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(kw));
	for (int pass = 0; pass < 2; pass++)
	{
		bool with_placement = (pass == 0);
		State attempt = Save();
		ParseNodePtr placement;
		if (with_placement)
		{
			placement = ParseNewPlacement();
			if (!placement)
				continue;
		}
		ParseNodePtr type = ParseNewTypeId();
		if (!type)
		{
			State paren_state = Save();
			if (MatchSimple(OP_LPAREN))
			{
				type = ParseTypeId();
				if (!type || !MatchSimple(OP_RPAREN))
				{
					type.reset();
					Restore(paren_state);
				}
			}
		}
		if (!type)
		{
			Restore(attempt);
			continue;
		}
		if (placement)
			node->Add(move(placement));
		node->Add(move(type));
		ParseNodePtr initializer = ParseNewInitializer();
		if (initializer)
			node->Add(move(initializer));
		return node;
	}
	Restore(state);
	return ParseNodePtr();
}

// new-placement: OP_LPAREN expression-list OP_RPAREN
ParseNodePtr Parser::ParseNewPlacement()
{
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return ParseNodePtr();
	ParseNodePtr arguments = ParseExpressionList();
	if (!arguments || !MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("new-placement");
	node->Add(move(arguments));
	return node;
}

// new-type-id: type-specifier-seq new-declarator?
ParseNodePtr Parser::ParseNewTypeId()
{
	ParseNodePtr specifiers = ParseTypeSpecifierSeq();
	if (!specifiers)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("new-type-id");
	node->Add(move(specifiers));
	ParseNodePtr declarator = ParseNewDeclarator();
	if (declarator)
		node->Add(move(declarator));
	return node;
}

// new-declarator: ptr-operator* noptr-new-declarator | ptr-operator+
ParseNodePtr Parser::ParseNewDeclarator()
{
	ParseNodePtr node = MakeParseNode("new-declarator");
	int num_ops = 0;
	for (;;)
	{
		ParseNodePtr op = ParsePtrOperator();
		if (!op)
			break;
		node->Add(move(op));
		num_ops++;
	}
	ParseNodePtr noptr = ParseNoptrNewDeclarator();
	if (noptr)
		node->Add(move(noptr));
	else if (num_ops == 0)
		return ParseNodePtr();
	return node;
}

// noptr-new-declarator: OP_LSQUARE expression OP_RSQUARE
//     attribute-specifier* (OP_LSQUARE constant-expression OP_RSQUARE
//     attribute-specifier*)*
ParseNodePtr Parser::ParseNoptrNewDeclarator()
{
	State state = Save();
	if (!MatchSimple(OP_LSQUARE))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("noptr-new-declarator");
	ParseNodePtr bound = ParseExpression();
	if (!bound || !MatchSimple(OP_RSQUARE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(bound));
	ParseAttributeSpecifiers(node.get());
	for (;;)
	{
		State extent_state = Save();
		if (!MatchSimple(OP_LSQUARE))
			break;
		ParseNodePtr extent = ParseConstantExpression();
		if (!extent || !MatchSimple(OP_RSQUARE))
		{
			Restore(extent_state);
			break;
		}
		node->Add(move(extent));
		ParseAttributeSpecifiers(node.get());
	}
	return node;
}

// new-initializer: OP_LPAREN expression-list? OP_RPAREN |
//     braced-init-list
ParseNodePtr Parser::ParseNewInitializer()
{
	if (AtSimple(OP_LBRACE))
	{
		ParseNodePtr braced = ParseBracedInitList();
		if (!braced)
			return ParseNodePtr();
		ParseNodePtr node = MakeParseNode("new-initializer");
		node->Add(move(braced));
		return node;
	}
	State state = Save();
	if (!MatchSimple(OP_LPAREN))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("new-initializer");
	if (!AtSimple(OP_RPAREN))
	{
		ParseNodePtr arguments = ParseExpressionList();
		if (!arguments)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(arguments));
	}
	if (!MatchSimple(OP_RPAREN))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// delete-expression: OP_COLON2? KW_DELETE cast-expression |
//     OP_COLON2? KW_DELETE OP_LSQUARE OP_RSQUARE cast-expression
ParseNodePtr Parser::ParseDeleteExpression()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("delete-expression");
	if (AtSimple(OP_COLON2))
		node->Add(MatchSimpleLeaf(OP_COLON2));
	ParseNodePtr kw = MatchSimpleLeaf(KW_DELETE);
	if (!kw)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(kw));
	State array_state = Save();
	if (MatchSimple(OP_LSQUARE) && MatchSimple(OP_RSQUARE))
	{
		ParseNodePtr operand = ParseCastExpression();
		if (operand)
		{
			node->Add(MakeParseNode("delete-array"));
			node->Add(move(operand));
			return node;
		}
	}
	Restore(array_state);
	ParseNodePtr operand = ParseCastExpression();
	if (!operand)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(operand));
	return node;
}

// cast-expression: unary-expression | cast-operator cast-expression
// The cast reading (a parenthesized type-id) is tried first and backs
// out to unary-expression, so `(C)(x)` is a cast while `(x)(y)` is a
// call (8.2).
ParseNodePtr Parser::ParseCastExpression()
{
	return MemoParse(kMemoCastExpression, &Parser::ParseCastExpressionRule);
}

ParseNodePtr Parser::ParseCastExpressionRule()
{
	State state = Save();
	if (MatchSimple(OP_LPAREN))
	{
		ParseNodePtr type = ParseTypeId();
		if (type && MatchSimple(OP_RPAREN))
		{
			ParseNodePtr operand = ParseCastExpression();
			if (operand)
			{
				ParseNodePtr node = MakeParseNode("cast-expression");
				node->Add(move(type));
				node->Add(move(operand));
				return node;
			}
		}
		Restore(state);
	}
	return ParseUnaryExpression();
}

// initializer-clause: assignment-expression | braced-init-list
ParseNodePtr Parser::ParseInitializerClause()
{
	if (AtSimple(OP_LBRACE))
		return ParseBracedInitList();
	return ParseAssignmentExpression();
}

ParseNodePtr Parser::ParseInitializerClauseDots()
{
	return ParseDotsItem("initializer-clause-dots",
	                     &Parser::ParseInitializerClause);
}

// initializer-list: initializer-clause-dots
//     (OP_COMMA initializer-clause-dots)*
ParseNodePtr Parser::ParseInitializerList()
{
	return ParseCommaList("initializer-list",
	                      &Parser::ParseInitializerClauseDots);
}

// braced-init-list: OP_LBRACE initializer-list OP_COMMA? OP_RBRACE |
//     OP_LBRACE OP_RBRACE
ParseNodePtr Parser::ParseBracedInitList()
{
	State state = Save();
	if (!MatchSimple(OP_LBRACE))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("braced-init-list");
	if (!AtSimple(OP_RBRACE))
	{
		ParseNodePtr list = ParseInitializerList();
		if (!list)
		{
			Restore(state);
			return ParseNodePtr();
		}
		node->Add(move(list));
		MatchSimple(OP_COMMA);
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}
