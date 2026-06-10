#include "parser.h"

using std::move;

Parser::Parser(const vector<ParseToken>& tokens)
	: tokens_(tokens), pos_(0),
	  rule_failed_(tokens.size() * kNumMemoRules * 2, false)
{
}

// Failure memoization for the rules ordered-choice retries re-descend
// through. A rule's outcome is a pure function of (rule, position,
// innermost-bracket-is-angle): a failed parse restores the lookahead
// and bracket stack, hard brackets and the rule's own angle pairs open
// and close inside the rule, and InAngleBrackets() is the only ambient
// state any rule reads. Without the memo, constructs whose readings
// the grammar disambiguates by trial (template-argument's
// type-id/constant-expression/id-expression, sizeof and typeid
// operands, statement vs declaration, abstract vs named parameter
// declarators) re-parse failing subtrees once per enclosing reading —
// exponential in nesting depth on unparseable input (a 8-deep failing
// template-argument nest took ~10s). Remembering failures bounds each
// slot to one full parse. Successes are not cached: a successful parse
// is consumed immediately by its caller, and ordered choice revisits a
// succeeding position only a constant number of times.
ParseNodePtr Parser::MemoParse(EMemoRule rule_index, ParseFn rule)
{
	size_t slot = (pos_ * kNumMemoRules + rule_index) * 2 +
	              (InAngleBrackets() ? 1 : 0);
	if (rule_failed_[slot])
		return ParseNodePtr();
	ParseNodePtr node = (this->*rule)();
	if (!node)
		rule_failed_[slot] = true;
	return node;
}

// translation-unit: declaration* ST_EOF
ParseNodePtr Parser::ParseTranslationUnit()
{
	State state = Save();
	ParseNodePtr unit = MakeParseNode("translation-unit");
	for (;;)
	{
		ParseNodePtr declaration = ParseDeclaration();
		if (!declaration)
			break;
		unit->Add(move(declaration));
	}
	if (!AtEof())
	{
		Restore(state);
		return ParseNodePtr();
	}
	unit->Add(MakeTokenLeaf(Peek()));
	return unit;
}

Parser::State Parser::Save() const
{
	State state;
	state.pos = pos_;
	state.bracket_depth = brackets_.size();
	return state;
}

void Parser::Restore(const State& state)
{
	pos_ = state.pos;
	// Hard brackets open and close within a single grammar alternative,
	// so a parse never pops below its entry depth; undoing a failed
	// parse only needs to drop the contexts it pushed.
	if (brackets_.size() > state.bracket_depth)
		brackets_.resize(state.bracket_depth);
}

const ParseToken& Parser::Peek(size_t ahead) const
{
	size_t index = pos_ + ahead;
	if (index >= tokens_.size())
		index = tokens_.size() - 1;  // ST_EOF (BuildParseTokens guarantee)
	return tokens_[index];
}

void Parser::Advance()
{
	const ParseToken& token = Peek();
	if (token.kind == PTOK_EOF)
		return;
	if (token.kind == PTOK_SIMPLE)
	{
		switch (token.simple_type)
		{
		case OP_LPAREN:
			brackets_.push_back('(');
			break;
		case OP_LSQUARE:
			brackets_.push_back('[');
			break;
		case OP_LBRACE:
			brackets_.push_back('{');
			break;
		case OP_RPAREN:
			if (!brackets_.empty() && brackets_.back() == '(')
				brackets_.pop_back();
			break;
		case OP_RSQUARE:
			if (!brackets_.empty() && brackets_.back() == '[')
				brackets_.pop_back();
			break;
		case OP_RBRACE:
			if (!brackets_.empty() && brackets_.back() == '{')
				brackets_.pop_back();
			break;
		default:
			break;
		}
	}
	pos_++;
}

bool Parser::AtSimple(ETokenType type, size_t ahead) const
{
	const ParseToken& token = Peek(ahead);
	return token.kind == PTOK_SIMPLE && token.simple_type == type;
}

bool Parser::AtIdentifier(size_t ahead) const
{
	return Peek(ahead).kind == PTOK_IDENTIFIER;
}

bool Parser::AtLiteral() const
{
	return Peek().kind == PTOK_LITERAL;
}

bool Parser::AtEof() const
{
	return Peek().kind == PTOK_EOF;
}

bool Parser::MatchSimple(ETokenType type)
{
	if (!AtSimple(type))
		return false;
	Advance();
	return true;
}

ParseNodePtr Parser::MatchSimpleLeaf(ETokenType type)
{
	if (!AtSimple(type))
		return ParseNodePtr();
	ParseNodePtr leaf = MakeTokenLeaf(Peek());
	Advance();
	return leaf;
}

ParseNodePtr Parser::MatchIdentifierLeaf()
{
	if (!AtIdentifier())
		return ParseNodePtr();
	ParseNodePtr leaf = MakeTokenLeaf(Peek());
	Advance();
	return leaf;
}

ParseNodePtr Parser::MatchLiteralLeaf()
{
	if (!AtLiteral())
		return ParseNodePtr();
	ParseNodePtr leaf = MakeTokenLeaf(Peek());
	Advance();
	return leaf;
}

bool Parser::InAngleBrackets() const
{
	return !brackets_.empty() && brackets_.back() == '<';
}

bool Parser::MatchOpenAngle()
{
	if (!MatchSimple(OP_LT))
		return false;
	brackets_.push_back('<');
	return true;
}

// close-angle-bracket: OP_GT | ST_RSHIFT_1 | ST_RSHIFT_2
// A split OP_RSHIFT closes two levels: the inner list consumes
// ST_RSHIFT_1 and the enclosing one ST_RSHIFT_2 (14.2.3).
ParseNodePtr Parser::ParseCloseAngleBracket()
{
	const ParseToken& token = Peek();
	bool closes = AtSimple(OP_GT) || token.kind == PTOK_RSHIFT_1 ||
	              token.kind == PTOK_RSHIFT_2;
	if (!closes)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("close-angle-bracket");
	node->Add(MakeTokenLeaf(token));
	Advance();
	if (!brackets_.empty() && brackets_.back() == '<')
		brackets_.pop_back();
	return node;
}

// element (OP_COMMA element)* with the trailing separator backed out
// when the next element fails (so `{1,2,}` and `(int, ...)` work).
ParseNodePtr Parser::ParseCommaList(const char* name, ParseFn element)
{
	ParseNodePtr first = (this->*element)();
	if (!first)
		return ParseNodePtr();
	ParseNodePtr list = MakeParseNode(name);
	list->Add(move(first));
	for (;;)
	{
		State state = Save();
		if (!MatchSimple(OP_COMMA))
			break;
		ParseNodePtr next = (this->*element)();
		if (!next)
		{
			Restore(state);
			break;
		}
		list->Add(move(next));
	}
	return list;
}

// element OP_DOTS? (the `foo-dots` items of the expansion-aware lists)
ParseNodePtr Parser::ParseDotsItem(const char* name, ParseFn element)
{
	ParseNodePtr inner = (this->*element)();
	if (!inner)
		return ParseNodePtr();
	ParseNodePtr item = MakeParseNode(name);
	item->Add(move(inner));
	if (AtSimple(OP_DOTS))
		item->Add(MatchSimpleLeaf(OP_DOTS));
	return item;
}
