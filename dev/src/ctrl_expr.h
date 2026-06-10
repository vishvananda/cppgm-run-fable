#pragma once

#include <ostream>
#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "post_token.h"

// Answers the `defined` operator in a controlling expression. PA3 wires
// the course mock; the preprocessor assignments wire the real macro
// table, so the evaluator never owns macro state.
typedef bool (*IsDefinedFn)(const string& identifier);

// Evaluates one conditional-inclusion controlling expression (16.1) given
// the line's preprocessing-tokens converted to PostTokens in
// identifier_or_keyword context (identifiers NOT folded to keywords).
// Returns the output line: a decimal intmax_t value, a decimal uintmax_t
// value with a `u` suffix, or "error". Per 16.2.4 every operand acts as
// intmax_t/uintmax_t; result signedness is computed statically over the
// whole tree (including non-evaluated ?: branches), while evaluation
// errors (division/modulus by zero, INTMAX_MIN/-1, shift count negative
// or >= 64) only count where evaluation actually reaches.
string EvaluateControllingExpression(const std::vector<PostToken>& tokens,
                                     IsDefinedFn is_defined);

// PA3 driver stream: splits the phase-3 preprocessing-token stream into
// logical lines at new-line tokens, converts each token (whitespace
// discarded, no string-literal concatenation -- a string is never an
// integral-literal), and writes one EvaluateControllingExpression result
// line per non-empty line, then "eof".
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
	void FinishLine();

	std::ostream& out_;
	IsDefinedFn is_defined_;
	std::vector<PostToken> line_;
};
