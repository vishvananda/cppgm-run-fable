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

bool IsIntegralLiteral(const PostToken& token)
{
	bool is_unsigned = false;
	return token.kind == PTK_LITERAL &&
		IntegralSignedness(token.type, is_unsigned);
}

// The operator/punctuator tokens that appear in the
// controlling-expression grammar. Every other simple token (comma,
// assignments, ++, new, ...) poisons the line, even after a complete
// expression -- the reference rejects `5 ,` but accepts `5 (`.
bool IsGrammarOperator(ETokenType type)
{
	switch (type)
	{
	case OP_LPAREN: case OP_RPAREN: case OP_QMARK: case OP_COLON:
	case OP_LOR: case OP_LAND: case OP_BOR: case OP_XOR: case OP_AMP:
	case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
	case OP_LSHIFT: case OP_RSHIFT: case OP_PLUS: case OP_MINUS:
	case OP_STAR: case OP_DIV: case OP_MOD: case OP_LNOT: case OP_COMPL:
		return true;
	default:
		return false;
	}
}

bool IsSimpleOp(const PostToken& token, ETokenType type)
{
	return token.kind == PTK_SIMPLE && token.token_type == type;
}

// Reference-pinned split for pp-numbers its integer parser rejects: a
// spelling with a dot, or with an exponent marker (e/E in decimal
// context, p/P once hexadecimal -- any x/X switches) directly followed
// by a digit or sign, reads as a botched floating literal and poisons
// only its line; every other invalid number defers the error while the
// unit carries into the next line.
bool InvalidNumberPoisonsLine(const string& spelling)
{
	bool hex = false;
	for (size_t i = 0; i < spelling.size(); i++)
	{
		char c = spelling[i];
		if (c == '.')
			return true;
		if (c == 'x' || c == 'X')
			hex = true;
		bool marker = hex ? c == 'p' || c == 'P' : c == 'e' || c == 'E';
		if (marker && i + 1 < spelling.size())
		{
			char next = spelling[i + 1];
			if ((next >= '0' && next <= '9') || next == '+' || next == '-')
				return true;
		}
	}
	return false;
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

// Hard grammar mismatches (unbalanced ( ) and ? :, unexpected end of
// the unit) and non-evaluable tokens; the unit reports "error".
struct ParseError {};

// Course-defined evaluation errors (division/modulus right operand zero,
// INTMAX_MIN over -1, shift count out of range); only raised where
// evaluation actually reaches, so short-circuited operands stay silent.
struct EvalError {};

struct Node;
typedef std::unique_ptr<Node> NodePtr;

// One controlling-expression AST node. kRecovery stands for a grammar
// operator the reference consumes where a primary-expression was
// required; it evaluates as a zero that is unsigned where evaluation
// reaches it.
struct Node
{
	enum Kind { kValue, kRecovery, kUnary, kBinary, kConditional };

	Node() : kind(kValue), is_unsigned(false), value(0), op(OP_PLUS) {}

	Kind kind;
	bool is_unsigned;          // kValue: the literal's promoted signedness
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

NodePtr MakeInner(Node::Kind kind, ETokenType op,
                  NodePtr a, NodePtr b, NodePtr c)
{
	NodePtr node(new Node);
	node->kind = kind;
	node->op = op;
	node->a = std::move(a);
	node->b = std::move(b);
	node->c = std::move(c);
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
// Reference-pinned deviations from the bare grammar: a grammar operator
// that cannot start a primary-expression is consumed as a kRecovery
// operand, running out of tokens mid-parse is a hard error, a mismatched
// ) or : is a hard error, and tokens left over after a complete
// top-level expression are ignored.
class Parser
{
public:
	explicit Parser(const std::vector<PostToken>& tokens)
		: tokens_(tokens), pos_(0)
	{}

	NodePtr Parse()
	{
		return ParseConditional();
	}

private:
	const PostToken* Peek() const
	{
		return pos_ < tokens_.size() ? &tokens_[pos_] : 0;
	}

	bool PeekOp(ETokenType type) const
	{
		const PostToken* token = Peek();
		return token && IsSimpleOp(*token, type);
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
		return MakeInner(Node::kConditional, OP_QMARK,
		                 std::move(cond), std::move(lhs), std::move(rhs));
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
			lhs = MakeInner(Node::kBinary, op,
			                std::move(lhs), std::move(rhs), NodePtr());
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
				return MakeInner(Node::kUnary, token->token_type,
				                 ParseUnary(), NodePtr(), NodePtr());
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
			// course-defined: true/false evaluate as 1/0; every other
			// identifier_or_keyword evaluates as (signed) 0 (defined was
			// folded away before the unit reached the parser)
			return MakeValue(token->source == "true" ? 1 : 0, false);
		}
		if (PeekOp(OP_LPAREN))
		{
			pos_++;
			NodePtr inner = ParseConditional();
			ExpectOp(OP_RPAREN);
			return inner;
		}
		if (token->kind != PTK_SIMPLE)
			throw ParseError();
		// A grammar operator that cannot start a primary-expression (the
		// calculator admits no other simple tokens into a unit); the
		// reference consumes it and substitutes a recovery zero.
		pos_++;
		return MakeInner(Node::kRecovery, OP_PLUS,
		                 NodePtr(), NodePtr(), NodePtr());
	}

	const std::vector<PostToken>& tokens_;
	size_t pos_;
};

// One evaluated sub-expression. The reference computes signedness in the
// same pass as the value, so a recovery zero is unsigned only where
// evaluation is enabled, and `recovery` marks an event that is still
// bubbling up: a binary operator whose right operand contains a pending
// recovery falls back to its left operand's signedness, comparisons and
// logical operators absorb pending events into their fresh signed
// result, and the conditional operator absorbs events from all three
// operands. `enabled` is false inside short-circuited operands and
// non-selected conditional branches: their literal types still propagate
// (260-cond-ret-type) but recovery zeros stay signed and course-defined
// evaluation errors stay silent.
struct EvalResult
{
	unsigned long long value;
	bool is_unsigned;
	// is_unsigned with pending recovery zeros discounted: comparisons
	// judge their operands by this (reference-pinned: `% - 9 > 0`
	// compares signed but `% - 9u > 0` compares unsigned)
	bool clean_unsigned;
	bool recovery;
};

EvalResult Evaluate(const Node& node, bool enabled);

EvalResult EvaluateUnary(const Node& node, bool enabled)
{
	EvalResult operand = Evaluate(*node.a, enabled);
	switch (node.op)
	{
	case OP_PLUS:
		break;
	case OP_MINUS:
		operand.value = 0 - operand.value;  // two's-complement wrap
		break;
	case OP_COMPL:
		operand.value = ~operand.value;
		break;
	default:  // OP_LNOT keeps the operand's signedness (reference-pinned)
		operand.value = operand.value == 0 ? 1 : 0;
		break;
	}
	return operand;
}

unsigned long long EvaluateDivMod(ETokenType op, unsigned long long lhs,
                                  unsigned long long rhs,
                                  bool operands_unsigned, bool enabled)
{
	if (rhs == 0)
	{
		if (enabled)
			throw EvalError();
		return 0;
	}
	if (operands_unsigned)
		return op == OP_DIV ? lhs / rhs : lhs % rhs;
	long long slhs = static_cast<long long>(lhs);
	long long srhs = static_cast<long long>(rhs);
	// INTMAX_MIN / -1 (and % -1) overflows; course-defined error
	if (slhs == LLONG_MIN && srhs == -1)
	{
		if (enabled)
			throw EvalError();
		return 0;
	}
	return static_cast<unsigned long long>(
		op == OP_DIV ? slhs / srhs : slhs % srhs);
}

unsigned long long EvaluateShift(ETokenType op, unsigned long long lhs,
                                 bool lhs_unsigned, const EvalResult& count,
                                 bool enabled)
{
	// the count is judged in the right operand's own type: negative (only
	// possible when signed) or >= 64 is a course-defined error
	bool bad_count = count.is_unsigned
		? count.value >= 64
		: static_cast<long long>(count.value) < 0 ||
		  static_cast<long long>(count.value) >= 64;
	if (bad_count)
	{
		if (enabled)
			throw EvalError();
		return 0;
	}
	if (op == OP_LSHIFT)
		return lhs << count.value;
	if (lhs_unsigned)
		return lhs >> count.value;
	// right shift of a signed operand sign-preserves (course-defined)
	return static_cast<unsigned long long>(
		static_cast<long long>(lhs) >> count.value);
}

unsigned long long LessThan(unsigned long long lhs, unsigned long long rhs,
                            bool operands_unsigned)
{
	if (operands_unsigned)
		return lhs < rhs ? 1 : 0;
	return static_cast<long long>(lhs) < static_cast<long long>(rhs) ? 1 : 0;
}

EvalResult EvaluateBinary(const Node& node, bool enabled)
{
	EvalResult lhs = Evaluate(*node.a, enabled);

	// && and || evaluate the right operand only when the left does not
	// already determine the result; both yield a fresh (signed) int and
	// absorb pending recovery events
	if (node.op == OP_LAND || node.op == OP_LOR)
	{
		bool decided = node.op == OP_LAND ? lhs.value == 0 : lhs.value != 0;
		EvalResult rhs = Evaluate(*node.b, enabled && !decided);
		unsigned long long value = node.op == OP_LAND
			? (lhs.value != 0 && rhs.value != 0 ? 1 : 0)
			: (lhs.value != 0 || rhs.value != 0 ? 1 : 0);
		EvalResult result = { value, false, false, false };
		return result;
	}

	EvalResult rhs = Evaluate(*node.b, enabled);
	// usual arithmetic conversions over intmax_t/uintmax_t (5.0.10); a
	// pending recovery in the right operand restores the left operand's
	// signedness (reference-pinned) and is absorbed here
	bool uns = rhs.recovery ? lhs.is_unsigned
	                        : lhs.is_unsigned || rhs.is_unsigned;
	unsigned long long value;
	bool result_unsigned = uns;
	bool result_clean = rhs.recovery
		? lhs.clean_unsigned
		: lhs.clean_unsigned || rhs.clean_unsigned;
	bool result_recovery = lhs.recovery;
	switch (node.op)
	{
	case OP_STAR: value = lhs.value * rhs.value; break;
	case OP_DIV:
	case OP_MOD:
		value = EvaluateDivMod(node.op, lhs.value, rhs.value, uns, enabled);
		break;
	case OP_PLUS: value = lhs.value + rhs.value; break;
	case OP_MINUS: value = lhs.value - rhs.value; break;
	case OP_LSHIFT:
	case OP_RSHIFT:
		// shifts take the promoted left operand's type (5.8)
		value = EvaluateShift(node.op, lhs.value, lhs.is_unsigned, rhs,
		                      enabled);
		result_unsigned = lhs.is_unsigned;
		result_clean = lhs.clean_unsigned;
		break;
	case OP_LT: case OP_GT: case OP_LE: case OP_GE:
	case OP_EQ: case OP_NE:
	{
		// comparisons yield a fresh (signed) int and absorb pending
		// recovery events; operands are judged with pending recovery
		// zeros discounted (reference-pinned: `% - 9 > 0` compares
		// signed, `% - 9u > 0` unsigned)
		bool cmp_uns = lhs.clean_unsigned || rhs.clean_unsigned;
		switch (node.op)
		{
		case OP_LT: value = LessThan(lhs.value, rhs.value, cmp_uns); break;
		case OP_GT: value = LessThan(rhs.value, lhs.value, cmp_uns); break;
		case OP_LE:
			value = LessThan(rhs.value, lhs.value, cmp_uns) ? 0 : 1;
			break;
		case OP_GE:
			value = LessThan(lhs.value, rhs.value, cmp_uns) ? 0 : 1;
			break;
		case OP_EQ: value = lhs.value == rhs.value ? 1 : 0; break;
		default: value = lhs.value != rhs.value ? 1 : 0; break;
		}
		result_unsigned = false;
		result_clean = false;
		result_recovery = false;
		break;
	}
	case OP_AMP: value = lhs.value & rhs.value; break;
	case OP_XOR: value = lhs.value ^ rhs.value; break;
	default: value = lhs.value | rhs.value; break;  // OP_BOR
	}
	EvalResult result = { value, result_unsigned, result_clean,
	                      result_recovery };
	return result;
}

EvalResult Evaluate(const Node& node, bool enabled)
{
	switch (node.kind)
	{
	case Node::kValue:
	{
		EvalResult result = { node.value, node.is_unsigned,
		                      node.is_unsigned, false };
		return result;
	}
	case Node::kRecovery:
	{
		EvalResult result = { 0, enabled, false, true };
		return result;
	}
	case Node::kUnary:
		return EvaluateUnary(node, enabled);
	case Node::kBinary:
		return EvaluateBinary(node, enabled);
	default:
	{
		// ?: walks both branches so literal signedness propagates even
		// from the branch evaluation does not select (260-cond-ret-type),
		// while that branch's evaluation errors and recovery zeros stay
		// inert; pending recovery events are absorbed
		EvalResult cond = Evaluate(*node.a, enabled);
		EvalResult lhs = Evaluate(*node.b, enabled && cond.value != 0);
		EvalResult rhs = Evaluate(*node.c, enabled && cond.value == 0);
		bool uns = lhs.is_unsigned || rhs.is_unsigned;
		EvalResult result = { cond.value != 0 ? lhs.value : rhs.value,
		                      uns, uns, false };
		return result;
	}
	}
}

// Parses and evaluates one complete evaluation unit.
CtrlExprResult EvaluateUnit(const std::vector<PostToken>& tokens)
{
	CtrlExprResult result;
	try
	{
		NodePtr root = Parser(tokens).Parse();
		EvalResult evaluated = Evaluate(*root, true);
		result.kind = CtrlExprResult::kValue;
		result.value = evaluated.value;
		result.is_unsigned = evaluated.is_unsigned;
	}
	catch (const ParseError&)
	{
		result.kind = CtrlExprResult::kError;
	}
	catch (const EvalError&)
	{
		result.kind = CtrlExprResult::kError;
	}
	return result;
}

// The PA3 output line for a value or error result.
string FormatResult(const CtrlExprResult& result)
{
	if (result.kind == CtrlExprResult::kError)
		return "error";
	std::ostringstream oss;
	if (result.is_unsigned)
		oss << result.value << 'u';
	else
		oss << static_cast<long long>(result.value);
	return oss.str();
}

PostToken MakeDefinedResult(bool defined)
{
	PostToken token;
	token.kind = PTK_LITERAL;
	token.type = FT_INT;
	token.source = "defined";
	token.data = LittleEndianBytes(defined ? 1 : 0, 4);
	return token;
}

} // namespace

CtrlExprCalculator::CtrlExprCalculator(IsDefinedFn is_defined)
	: is_defined_(is_defined), active_(false), skip_(false),
	  error_pending_(false), defined_(kNoDefined), defined_paren_(false)
{}

void CtrlExprCalculator::Token(TokenCategory category, const PostToken& token)
{
	active_ = true;
	// an open defined operator consumes every token, bypassing both the
	// skip poison and per-token analysis (reference-pinned)
	if (defined_ != kNoDefined)
	{
		FeedDefined(category, token);
		return;
	}
	if (skip_)
	{
		// a poisoned line still opens defined operators; their dangling
		// state carries the error into the next line
		if (category == kIdentifier && token.source == "defined")
		{
			defined_ = kWantOperand;
			defined_paren_ = false;
		}
		return;
	}
	switch (category)
	{
	case kNumber:
		if (token.kind == PTK_INVALID)
		{
			// invalid integers report error at the new-line while the
			// unit (prefix and following tokens) survives and carries
			// into the next line; float-shaped spellings poison the
			// line like a real floating literal
			if (InvalidNumberPoisonsLine(token.source))
				skip_ = true;
			else
				error_pending_ = true;
		}
		else if (IsIntegralLiteral(token))
			unit_.push_back(token);
		else
			skip_ = true;  // floating or user-defined
		break;
	case kCharLiteral:
		if (IsIntegralLiteral(token))
			unit_.push_back(token);
		else
			skip_ = true;  // invalid or user-defined
		break;
	case kIdentifier:
		if (token.source == "defined")
		{
			defined_ = kWantOperand;
			defined_paren_ = false;
		}
		else
			unit_.push_back(token);
		break;
	case kOpOrPunc:
		if (token.kind == PTK_SIMPLE && IsGrammarOperator(token.token_type))
			unit_.push_back(token);
		else
			skip_ = true;  // outside the grammar (or # ## %: %:%:)
		break;
	default:  // kNonEvaluable
		skip_ = true;
		break;
	}
}

// Reference-pinned malformed-defined handling: any token that does not
// advance the construct defers an error to the line's new-line and is
// consumed without analysis, while the construct keeps waiting -- a
// later identifier still folds (`defined ( 3.5 a ) 6` evaluates `1 6`)
// and a later `)` still closes the parenthesized form.
void CtrlExprCalculator::FeedDefined(TokenCategory category,
                                     const PostToken& token)
{
	if (defined_ == kWantOperand)
	{
		if (category == kIdentifier)
		{
			unit_.push_back(MakeDefinedResult(is_defined_(token.source)));
			defined_ = defined_paren_ ? kWantRParen : kNoDefined;
		}
		else if (!defined_paren_ && IsSimpleOp(token, OP_LPAREN))
			defined_paren_ = true;
		else
			error_pending_ = true;
		return;
	}
	// kWantRParen
	if (IsSimpleOp(token, OP_RPAREN))
		defined_ = kNoDefined;
	else
		error_pending_ = true;
}

bool CtrlExprCalculator::FinishLine(string& output)
{
	CtrlExprResult result;
	if (!FinishLineResult(result))
		return false;
	output = FormatResult(result);
	return true;
}

bool CtrlExprCalculator::FinishLineResult(CtrlExprResult& result)
{
	// a line that contributed no tokens prints nothing; pending state
	// (carried unit, skip poison) passes over it untouched
	if (!active_)
		return false;
	active_ = false;
	if (defined_ != kNoDefined)
	{
		// a defined operator left open reports error; anything already
		// folded or accumulated stays in the carried unit
		error_pending_ = true;
		defined_ = kNoDefined;
	}
	if (error_pending_)
	{
		// the unit (and a pending skip poison) survives into the next line
		error_pending_ = false;
		result.kind = CtrlExprResult::kError;
		return true;
	}
	if (skip_)
	{
		skip_ = false;
		unit_.clear();
		result.kind = CtrlExprResult::kError;
		return true;
	}
	if (unit_.empty())
		return false;
	std::vector<PostToken> unit;
	unit.swap(unit_);
	result = EvaluateUnit(unit);
	return true;
}

void CtrlExprCalculator::Reset()
{
	unit_.clear();
	active_ = false;
	skip_ = false;
	error_pending_ = false;
	defined_ = kNoDefined;
	defined_paren_ = false;
}

string EvaluateControllingExpression(const std::vector<PostToken>& tokens,
                                     IsDefinedFn is_defined)
{
	CtrlExprResult result = EvaluateControllingResult(tokens, is_defined);
	if (result.kind == CtrlExprResult::kNone)
		return "";
	return FormatResult(result);
}

CtrlExprResult EvaluateControllingResult(const std::vector<PostToken>& tokens,
                                         IsDefinedFn is_defined)
{
	CtrlExprCalculator calc(is_defined);
	for (size_t i = 0; i < tokens.size(); i++)
	{
		const PostToken& token = tokens[i];
		CtrlExprCalculator::TokenCategory category;
		switch (token.kind)
		{
		case PTK_LITERAL:
		case PTK_UD_INTEGER:
		case PTK_UD_FLOATING:
			category = CtrlExprCalculator::kNumber;
			break;
		case PTK_UD_CHARACTER:
			category = CtrlExprCalculator::kCharLiteral;
			break;
		case PTK_IDENTIFIER:
			category = CtrlExprCalculator::kIdentifier;
			break;
		case PTK_SIMPLE:
			category = CtrlExprCalculator::kOpOrPunc;
			break;
		default:
			// strings and tokens whose provenance is lost (PTK_INVALID);
			// within a single sequence every non-evaluable token reports
			// the same "error"
			category = CtrlExprCalculator::kNonEvaluable;
			break;
		}
		calc.Token(category, token);
	}
	CtrlExprResult result;
	calc.FinishLineResult(result);
	return result;
}

CtrlExprStream::CtrlExprStream(std::ostream& out, IsDefinedFn is_defined)
	: out_(out), calc_(is_defined)
{}

void CtrlExprStream::emit_whitespace_sequence()
{
	// whitespace-sequences are discarded (PA3 Features)
}

void CtrlExprStream::emit_new_line()
{
	string output;
	if (calc_.FinishLine(output))
		out_ << output << "\n";
}

void CtrlExprStream::emit_header_name(const string& data)
{
	calc_.Token(CtrlExprCalculator::kNonEvaluable, MakeInvalidToken(data));
}

void CtrlExprStream::emit_identifier(const string& data)
{
	// identifier_or_keyword context: PA1 identifier semantics, so no
	// keyword folding; true/false are resolved by the parser and defined
	// is folded by the calculator
	PostToken token;
	token.kind = PTK_IDENTIFIER;
	token.source = data;
	calc_.Token(CtrlExprCalculator::kIdentifier, token);
}

void CtrlExprStream::emit_pp_number(const string& data)
{
	calc_.Token(CtrlExprCalculator::kNumber, AnalyzePPNumber(data));
}

void CtrlExprStream::emit_character_literal(const string& data)
{
	calc_.Token(CtrlExprCalculator::kCharLiteral,
	            AnalyzeCharLiteral(data).token);
}

void CtrlExprStream::emit_user_defined_character_literal(const string& data)
{
	// yields a PTK_UD_CHARACTER (or invalid) token; never integral
	calc_.Token(CtrlExprCalculator::kCharLiteral,
	            AnalyzeCharLiteral(data).token);
}

void CtrlExprStream::emit_string_literal(const string& data)
{
	// a string literal is an array of some character type, never an
	// integral-literal, so no phase 6/7 string analysis is needed here
	calc_.Token(CtrlExprCalculator::kNonEvaluable, MakeInvalidToken(data));
}

void CtrlExprStream::emit_user_defined_string_literal(const string& data)
{
	calc_.Token(CtrlExprCalculator::kNonEvaluable, MakeInvalidToken(data));
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
	calc_.Token(CtrlExprCalculator::kOpOrPunc, token);
}

void CtrlExprStream::emit_non_whitespace_char(const string& data)
{
	calc_.Token(CtrlExprCalculator::kNonEvaluable, MakeInvalidToken(data));
}

void CtrlExprStream::emit_eof()
{
	// Only the \UFFFFFFFF end-of-input marker can cut a line short of
	// its new-line (phase 1-2 appends a missing final line feed); the
	// reference discards such a partial line rather than evaluating it.
	calc_.Reset();
	out_ << "eof\n";
}
