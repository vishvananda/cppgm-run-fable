#pragma once

#include <functional>
#include <ostream>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "post_token.h"

// Answers the `defined` operator in a controlling expression. PA3 wires
// the course mock; the preprocessor assignments close over the real
// macro table, so the evaluator never owns macro state.
typedef std::function<bool(const string& identifier)> IsDefinedFn;

// Typed outcome of one controlling-expression evaluation. kNone: the
// token sequence formed no unit (empty). kError: the PA3 "error"
// outcome. kValue: the 64-bit promoted value and its signedness.
struct CtrlExprResult
{
	enum Kind { kNone, kError, kValue };

	CtrlExprResult() : kind(kNone), value(0), is_unsigned(false) {}

	Kind kind;
	unsigned long long value;
	bool is_unsigned;
};

// Streaming evaluator for conditional-inclusion controlling expressions
// (16.1), pinned to the reference binary's semantics (the pinned rules
// are catalogued in pa3/plan.md):
//
// - Tokens accumulate into an evaluation unit; `defined id` and
//   `defined ( id )` fold to a signed 1/0 literal as tokens arrive.
// - A non-evaluable token (floating or user-defined literal, string,
//   invalid character literal, operator outside the grammar, stray
//   character) poisons the rest of the line: the line prints `error`
//   and the unit restarts empty.
// - An invalid pp-number empties the unit and defers an `error` to the
//   line's new-line, but tokens after it keep accumulating and carry
//   into the NEXT line's unit (reference behavior: the error line's
//   remainder and the following line evaluate as one unit).
// - At a new-line: a dangling `defined` operand is a self-contained
//   `error`; a missing `)` of `defined (id` reports `error` but keeps
//   the folded value in the carried unit.
// - A complete unit evaluates with the reference's recovery semantics:
//   a grammar operator where a primary was required is consumed as a
//   zero that is unsigned where evaluation is enabled; tokens after a
//   complete top-level expression are ignored; running out of tokens
//   mid-parse is an error. Per 16.2.4 every operand acts as
//   intmax_t/uintmax_t; signedness propagates from non-evaluated ?:
//   branches, while course-defined evaluation errors (division/modulus
//   by zero, INTMAX_MIN/-1, shift count negative or >= 64) only count
//   where evaluation actually reaches.
class CtrlExprCalculator
{
public:
	// The PA1 emit channel a token arrived through. The analyzed
	// PostToken alone is not enough: an invalid pp-number carries the
	// unit across lines while an invalid character literal only poisons
	// its own line.
	enum TokenCategory
	{
		kNumber,        // pp-number, analyzed (integer/floating/UD/invalid)
		kCharLiteral,   // character-literal, analyzed (incl. UD/invalid)
		kIdentifier,
		kOpOrPunc,      // analyzed op (PTK_SIMPLE or invalid # ## %: %:%:)
		kNonEvaluable   // strings, header-names, stray characters
	};

	explicit CtrlExprCalculator(IsDefinedFn is_defined);

	void Token(TokenCategory category, const PostToken& token);

	// Ends the logical line. Returns true and sets output when the line
	// produces one (a value or "error"); empty lines produce none.
	bool FinishLine(string& output);

	// Typed form of FinishLine (kError replaces "error" output).
	bool FinishLineResult(CtrlExprResult& result);

	// Discards all pending state (a partial line at end of input is
	// dropped, matching the reference).
	void Reset();

private:
	enum DefinedState { kNoDefined, kWantOperand, kWantRParen };

	void FeedDefined(TokenCategory category, const PostToken& token);

	IsDefinedFn is_defined_;
	std::vector<PostToken> unit_;
	bool active_;         // line contributed tokens; token-less lines
	                      // print nothing and pending state passes over
	bool skip_;           // poisoned line: drop tokens until new-line
	bool error_pending_;  // invalid pp-number: error at new-line, unit carries
	DefinedState defined_;
	bool defined_paren_;
};

// Evaluates one controlling expression given its converted tokens (PA3
// identifier_or_keyword context: identifiers NOT folded to keywords).
// Returns the output line -- a decimal intmax_t value, a decimal
// uintmax_t value with a `u` suffix, or "error" -- or "" for an empty
// token sequence.
string EvaluateControllingExpression(const std::vector<PostToken>& tokens,
                                     IsDefinedFn is_defined);

// Typed form of EvaluateControllingExpression, so the preprocessor
// assignments read #if/#elif truth from the evaluated value instead of
// re-parsing the PA3 output line.
CtrlExprResult EvaluateControllingResult(const std::vector<PostToken>& tokens,
                                         IsDefinedFn is_defined);

// PA3 driver stream: converts each phase-3 preprocessing-token in
// identifier_or_keyword context (whitespace discarded, no string
// concatenation -- a string is never an integral-literal) and feeds the
// streaming calculator, writing one result line per logical line that
// produces one, then "eof".
class CtrlExprStream : public IPPTokenStream
{
public:
	CtrlExprStream(std::ostream& out, IsDefinedFn is_defined);

	void emit_whitespace_sequence();
	void emit_new_line();
	void emit_header_name(const string& data);
	void emit_identifier(const string& data);
	void emit_pp_number(const string& data);
	void emit_character_literal(const string& data);
	void emit_user_defined_character_literal(const string& data);
	void emit_string_literal(const string& data);
	void emit_user_defined_string_literal(const string& data);
	void emit_preprocessing_op_or_punc(const string& data);
	void emit_non_whitespace_char(const string& data);
	void emit_eof();

private:
	std::ostream& out_;
	CtrlExprCalculator calc_;
};
