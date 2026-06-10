#include "ctrl_expr.h"

#include <climits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "numeric_literals.h"
#include "text_literals.h"

namespace {

// Course-defined signedness of the integral fundamental types under the
// 16.2.4 promotion to intmax_t/uintmax_t. Returns false for types that
// are not integral (floating, void, nullptr_t), which cannot appear in a
// controlling expression.
bool IntegralSignedness(EFundamentalType type, bool& is_unsigned)
{
	switch (type)
	{
	case FT_SIGNED_CHAR:
	case FT_SHORT_INT:
	case FT_INT:
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_WCHAR_T:
	case FT_CHAR:
	case FT_BOOL:
		is_unsigned = false;
		return true;
	case FT_UNSIGNED_CHAR:
	case FT_UNSIGNED_SHORT_INT:
	case FT_UNSIGNED_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
	case FT_CHAR16_T:
	case FT_CHAR32_T:
		is_unsigned = true;
		return true;
	default:
		return false;
	}
}

// Widens the little-endian ABI value bytes of an integral literal to the
// 64-bit promoted value: sign-extended for signed types, zero-extended
// for unsigned types (course-defined promotion behavior).
unsigned long long WidenLiteralValue(const string& data, bool is_unsigned)
{
	unsigned long long value = 0;
	for (size_t i = data.size(); i-- > 0;)
		value = (value << 8) | static_cast<unsigned char>(data[i]);
	size_t bits = data.size() * 8;
	if (!is_unsigned && bits > 0 && bits < 64 && ((value >> (bits - 1)) & 1))
		value |= ~0ULL << bits;
	return value;
}

// Grammar mismatches and disallowed tokens; the line reports "error".
struct ParseError {};

// Course-defined evaluation errors (division/modulus right operand zero,
// INTMAX_MIN over -1, shift count out of range); only raised where
// evaluation actually reaches, so short-circuited operands stay silent.
struct EvalError {};

struct Node;
typedef std::unique_ptr<Node> NodePtr;

// One controlling-expression AST node. Every node's value acts as
// intmax_t or uintmax_t (16.2.4); is_unsigned is the statically computed
// choice, present even for branches evaluation never reaches.
struct Node
{
	enum Kind { kValue, kUnary, kBinary, kConditional };

	Node() : kind(kValue), is_unsigned(false), value(0), op(OP_PLUS) {}

	Kind kind;
	bool is_unsigned;
	unsigned long long value;  // kValue: the 64-bit promoted bit pattern
	ETokenType op;             // kUnary/kBinary
	NodePtr a, b, c;
};

NodePtr MakeValue(unsigned long long value, bool is_unsigned)
{
	NodePtr node(new Node);
	node->kind = Node::kValue;
	node->is_unsigned = is_unsigned;
	node->value = value;
	return node;
}

NodePtr MakeUnary(ETokenType op, NodePtr operand)
{
	NodePtr node(new Node);
	node->kind = Node::kUnary;
	node->op = op;
	// ! yields (signed) int; + - ~ keep the promoted operand type
	node->is_unsigned = op == OP_LNOT ? false : operand->is_unsigned;
	node->a = std::move(operand);
	return node;
}

bool BinaryResultUnsigned(ETokenType op, const Node& lhs, const Node& rhs)
{
	switch (op)
	{
	// usual arithmetic conversions over intmax_t/uintmax_t (5.0.10)
	case OP_STAR:
	case OP_DIV:
	case OP_MOD:
	case OP_PLUS:
	case OP_MINUS:
	case OP_AMP:
	case OP_XOR:
	case OP_BOR:
		return lhs.is_unsigned || rhs.is_unsigned;
	// shifts take the promoted left operand's type (5.8)
	case OP_LSHIFT:
	case OP_RSHIFT:
		return lhs.is_unsigned;
	// comparisons and logical operators yield (signed) int
	default:
		return false;
	}
}

NodePtr MakeBinary(ETokenType op, NodePtr lhs, NodePtr rhs)
{
	NodePtr node(new Node);
	node->kind = Node::kBinary;
	node->op = op;
	node->is_unsigned = BinaryResultUnsigned(op, *lhs, *rhs);
	node->a = std::move(lhs);
	node->b = std::move(rhs);
	return node;
}

NodePtr MakeConditional(NodePtr cond, NodePtr lhs, NodePtr rhs)
{
	NodePtr node(new Node);
	node->kind = Node::kConditional;
	// usual arithmetic conversions across both branches, independent of
	// which branch evaluation later selects
	node->is_unsigned = lhs->is_unsigned || rhs->is_unsigned;
	node->a = std::move(cond);
	node->b = std::move(lhs);
	node->c = std::move(rhs);
	return node;
}

// Binary precedence levels, loosest binding first: the grammar ladder
// from logical-or-expression down to multiplicative-expression. -1 means
// the operator is not a controlling-expression binary operator.
const int kNumBinaryLevels = 10;

int BinaryLevel(ETokenType op)
{
	switch (op)
	{
	case OP_LOR: return 0;
	case OP_LAND: return 1;
	case OP_BOR: return 2;
	case OP_XOR: return 3;
	case OP_AMP: return 4;
	case OP_EQ: case OP_NE: return 5;
	case OP_LT: case OP_GT: case OP_LE: case OP_GE: return 6;
	case OP_LSHIFT: case OP_RSHIFT: return 7;
	case OP_PLUS: case OP_MINUS: return 8;
	case OP_STAR: case OP_DIV: case OP_MOD: return 9;
	default: return -1;
	}
}

// Hand-written predictive top-down parser for the PA3
// controlling-expression grammar. Left recursion in the binary ladder is
// handled by iteration, building leftward for left associativity.
class Parser
{
public:
	Parser(const std::vector<PostToken>& tokens, IsDefinedFn is_defined)
		: tokens_(tokens), is_defined_(is_defined), pos_(0)
	{}

	NodePtr Parse()
	{
		NodePtr root = ParseConditional();
		if (pos_ != tokens_.size())
			throw ParseError();
		return root;
	}

private:
	const PostToken* Peek() const
	{
		return pos_ < tokens_.size() ? &tokens_[pos_] : 0;
	}

	bool PeekOp(ETokenType type) const
	{
		const PostToken* token = Peek();
		return token && token->kind == PTK_SIMPLE && token->token_type == type;
	}

	void ExpectOp(ETokenType type)
	{
		if (!PeekOp(type))
			throw ParseError();
		pos_++;
	}

	NodePtr ParseConditional()
	{
		NodePtr cond = ParseBinary(0);
		if (!PeekOp(OP_QMARK))
			return cond;
		pos_++;
		NodePtr lhs = ParseConditional();
		ExpectOp(OP_COLON);
		NodePtr rhs = ParseConditional();
		return MakeConditional(std::move(cond), std::move(lhs), std::move(rhs));
	}

	NodePtr ParseBinary(int level)
	{
		if (level == kNumBinaryLevels)
			return ParseUnary();
		NodePtr lhs = ParseBinary(level + 1);
		while (Peek() && Peek()->kind == PTK_SIMPLE &&
		       BinaryLevel(Peek()->token_type) == level)
		{
			ETokenType op = Peek()->token_type;
			pos_++;
			NodePtr rhs = ParseBinary(level + 1);
			lhs = MakeBinary(op, std::move(lhs), std::move(rhs));
		}
		return lhs;
	}

	NodePtr ParseUnary()
	{
		const PostToken* token = Peek();
		if (token && token->kind == PTK_SIMPLE)
		{
			switch (token->token_type)
			{
			case OP_PLUS:
			case OP_MINUS:
			case OP_LNOT:
			case OP_COMPL:
				pos_++;
				return MakeUnary(token->token_type, ParseUnary());
			default:
				break;
			}
		}
		return ParsePrimary();
	}

	NodePtr ParsePrimary()
	{
		const PostToken* token = Peek();
		if (!token)
			throw ParseError();
		if (token->kind == PTK_LITERAL)
		{
			bool is_unsigned = false;
			if (!IntegralSignedness(token->type, is_unsigned))
				throw ParseError();
			pos_++;
			return MakeValue(WidenLiteralValue(token->data, is_unsigned),
			                 is_unsigned);
		}
		if (token->kind == PTK_IDENTIFIER)
		{
			pos_++;
			if (token->source == "defined")
				return ParseDefinedOperand();
			// course-defined: true/false evaluate as 1/0; every other
			// identifier_or_keyword evaluates as (signed) 0
			return MakeValue(token->source == "true" ? 1 : 0, false);
		}
		if (PeekOp(OP_LPAREN))
		{
			pos_++;
			NodePtr inner = ParseConditional();
			ExpectOp(OP_RPAREN);
			return inner;
		}
		throw ParseError();
	}

	// `defined` itself is consumed; accepts `defined id` and
	// `defined ( id )` where id is any identifier_or_keyword
	NodePtr ParseDefinedOperand()
	{
		const PostToken* token = Peek();
		if (token && token->kind == PTK_IDENTIFIER)
		{
			pos_++;
			return MakeValue(is_defined_(token->source) ? 1 : 0, false);
		}
		ExpectOp(OP_LPAREN);
		const PostToken* name = Peek();
		if (!name || name->kind != PTK_IDENTIFIER)
			throw ParseError();
		pos_++;
		ExpectOp(OP_RPAREN);
		return MakeValue(is_defined_(name->source) ? 1 : 0, false);
	}

	const std::vector<PostToken>& tokens_;
	IsDefinedFn is_defined_;
	size_t pos_;
};

unsigned long long Evaluate(const Node& node);

unsigned long long EvaluateUnary(const Node& node)
{
	unsigned long long value = Evaluate(*node.a);
	switch (node.op)
	{
	case OP_PLUS: return value;
	case OP_MINUS: return 0 - value;  // two's-complement wrap
	case OP_COMPL: return ~value;
	default: return value == 0 ? 1 : 0;  // OP_LNOT
	}
}

unsigned long long EvaluateDivMod(ETokenType op, unsigned long long lhs,
                                  unsigned long long rhs,
                                  bool operands_unsigned)
{
	if (rhs == 0)
		throw EvalError();
	if (operands_unsigned)
		return op == OP_DIV ? lhs / rhs : lhs % rhs;
	long long slhs = static_cast<long long>(lhs);
	long long srhs = static_cast<long long>(rhs);
	// INTMAX_MIN / -1 (and % -1) overflows; course-defined error
	if (slhs == LLONG_MIN && srhs == -1)
		throw EvalError();
	return static_cast<unsigned long long>(
		op == OP_DIV ? slhs / srhs : slhs % srhs);
}

unsigned long long EvaluateShift(const Node& node, unsigned long long lhs,
                                 unsigned long long rhs)
{
	// the count is judged in the right operand's own type: negative (only
	// possible when signed) or >= 64 is a course-defined error
	bool bad_count = node.b->is_unsigned
		? rhs >= 64
		: static_cast<long long>(rhs) < 0 ||
		  static_cast<long long>(rhs) >= 64;
	if (bad_count)
		throw EvalError();
	if (node.op == OP_LSHIFT)
		return lhs << rhs;
	if (node.a->is_unsigned)
		return lhs >> rhs;
	// right shift of a signed operand sign-preserves (course-defined)
	return static_cast<unsigned long long>(static_cast<long long>(lhs) >> rhs);
}

unsigned long long LessThan(unsigned long long lhs, unsigned long long rhs,
                            bool operands_unsigned)
{
	if (operands_unsigned)
		return lhs < rhs ? 1 : 0;
	return static_cast<long long>(lhs) < static_cast<long long>(rhs) ? 1 : 0;
}

unsigned long long EvaluateBinary(const Node& node)
{
	// && and || evaluate the right operand only when the left does not
	// already determine the result
	if (node.op == OP_LAND)
		return Evaluate(*node.a) == 0 ? 0 : (Evaluate(*node.b) != 0 ? 1 : 0);
	if (node.op == OP_LOR)
		return Evaluate(*node.a) != 0 ? 1 : (Evaluate(*node.b) != 0 ? 1 : 0);

	unsigned long long lhs = Evaluate(*node.a);
	unsigned long long rhs = Evaluate(*node.b);
	bool uns = node.a->is_unsigned || node.b->is_unsigned;
	switch (node.op)
	{
	case OP_STAR: return lhs * rhs;
	case OP_DIV:
	case OP_MOD: return EvaluateDivMod(node.op, lhs, rhs, uns);
	case OP_PLUS: return lhs + rhs;
	case OP_MINUS: return lhs - rhs;
	case OP_LSHIFT:
	case OP_RSHIFT: return EvaluateShift(node, lhs, rhs);
	case OP_LT: return LessThan(lhs, rhs, uns);
	case OP_GT: return LessThan(rhs, lhs, uns);
	case OP_LE: return LessThan(rhs, lhs, uns) ? 0 : 1;
	case OP_GE: return LessThan(lhs, rhs, uns) ? 0 : 1;
	case OP_EQ: return lhs == rhs ? 1 : 0;
	case OP_NE: return lhs != rhs ? 1 : 0;
	case OP_AMP: return lhs & rhs;
	case OP_XOR: return lhs ^ rhs;
	default: return lhs | rhs;  // OP_BOR
	}
}

unsigned long long Evaluate(const Node& node)
{
	switch (node.kind)
	{
	case Node::kValue: return node.value;
	case Node::kUnary: return EvaluateUnary(node);
	case Node::kBinary: return EvaluateBinary(node);
	default:
		// ?: evaluates only the selected branch; the result is already
		// the right 64-bit pattern under the statically chosen type
		return Evaluate(*node.a) != 0 ? Evaluate(*node.b) : Evaluate(*node.c);
	}
}

// 16.1/PA3 token check: every token must be an identifier, a grammar
// operator, or a non-array literal of integral type.
bool LineTokensEvaluable(const std::vector<PostToken>& tokens)
{
	for (size_t i = 0; i < tokens.size(); i++)
	{
		const PostToken& token = tokens[i];
		if (token.kind == PTK_IDENTIFIER || token.kind == PTK_SIMPLE)
			continue;
		bool is_unsigned = false;
		if (token.kind == PTK_LITERAL &&
		    IntegralSignedness(token.type, is_unsigned))
			continue;
		return false;
	}
	return true;
}

} // namespace

string EvaluateControllingExpression(const std::vector<PostToken>& tokens,
                                     IsDefinedFn is_defined)
{
	try
	{
		if (!LineTokensEvaluable(tokens))
			throw ParseError();
		NodePtr root = Parser(tokens, is_defined).Parse();
		unsigned long long value = Evaluate(*root);
		std::ostringstream oss;
		if (root->is_unsigned)
			oss << value << 'u';
		else
			oss << static_cast<long long>(value);
		return oss.str();
	}
	catch (const ParseError&)
	{
		return "error";
	}
	catch (const EvalError&)
	{
		return "error";
	}
}

CtrlExprStream::CtrlExprStream(std::ostream& out, IsDefinedFn is_defined)
	: out_(out), is_defined_(is_defined)
{}

void CtrlExprStream::emit_whitespace_sequence()
{
	// whitespace-sequences are discarded (PA3 Features)
}

void CtrlExprStream::emit_new_line()
{
	FinishLine();
}

void CtrlExprStream::emit_header_name(const string& data)
{
	line_.push_back(MakeInvalidToken(data));
}

void CtrlExprStream::emit_identifier(const string& data)
{
	// identifier_or_keyword context: PA1 identifier semantics, so no
	// keyword folding; true/false/defined are resolved by the parser
	PostToken token;
	token.kind = PTK_IDENTIFIER;
	token.source = data;
	line_.push_back(token);
}

void CtrlExprStream::emit_pp_number(const string& data)
{
	line_.push_back(AnalyzePPNumber(data));
}

void CtrlExprStream::emit_character_literal(const string& data)
{
	line_.push_back(AnalyzeCharLiteral(data).token);
}

void CtrlExprStream::emit_user_defined_character_literal(const string& data)
{
	// yields a PTK_UD_CHARACTER (or invalid) token; never integral
	line_.push_back(AnalyzeCharLiteral(data).token);
}

void CtrlExprStream::emit_string_literal(const string& data)
{
	// a string literal is an array of some character type, never an
	// integral-literal; the kind alone makes the line report error, so
	// no phase 6/7 string analysis is needed here
	PostToken token;
	token.kind = PTK_LITERAL_ARRAY;
	token.source = data;
	line_.push_back(token);
}

void CtrlExprStream::emit_user_defined_string_literal(const string& data)
{
	PostToken token;
	token.kind = PTK_UD_STRING;
	token.source = data;
	line_.push_back(token);
}

void CtrlExprStream::emit_preprocessing_op_or_punc(const string& data)
{
	PostToken token;
	token.source = data;
	// #, ##, %: and %:%: have no phase-7 mapping and stay invalid
	if (LookupSimpleTokenType(data, token.token_type))
		token.kind = PTK_SIMPLE;
	else
		token.kind = PTK_INVALID;
	line_.push_back(token);
}

void CtrlExprStream::emit_non_whitespace_char(const string& data)
{
	line_.push_back(MakeInvalidToken(data));
}

void CtrlExprStream::emit_eof()
{
	// Only the \UFFFFFFFF end-of-input marker can cut a line short of
	// its new-line (phase 1-2 appends a missing final line feed); the
	// reference discards such a partial line rather than evaluating it.
	line_.clear();
	out_ << "eof\n";
}

void CtrlExprStream::FinishLine()
{
	if (line_.empty())
		return;
	std::vector<PostToken> line;
	line.swap(line_);
	out_ << EvaluateControllingExpression(line, is_defined_) << "\n";
}
