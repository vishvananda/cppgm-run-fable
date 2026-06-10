#pragma once

#include <string>
#include <vector>

using std::string;

#include "post_token.h"

// Phase 7 analysis of a character-literal or
// user-defined-character-literal preprocessing-token (2.14.3): decodes
// escape-sequences, enforces the course rule of exactly one code point in
// the valid Unicode range, and types the value (char/int for ordinary
// literals, one code unit of char16_t/char32_t/wchar_t for u/U/L).
//
// body_invalid distinguishes the reference's two failure stages: a body
// that does not decode to one in-range code point is reported immediately
// and does NOT terminate a pending string-literal concatenation sequence,
// while ud-suffix and code-unit-width failures behave like any other
// token (the sequence is flushed first).
struct CharLiteralAnalysis
{
	PostToken token;
	bool body_invalid;
};

CharLiteralAnalysis AnalyzeCharLiteral(const string& source);

// Phases 6 and 7 for a maximal sequence of adjacent string-literal and
// user-defined-string-literal preprocessing-tokens (2.14.5, 2.14.8),
// with the reference's check order: more than one distinct
// encoding-prefix or more than one distinct ud-suffix makes the sequence
// one invalid token before any encoding happens; then the concatenated
// code points (terminating 0 appended, no range check: lone surrogates
// encode as-is) are encoded as UTF-8/16/32 per the effective prefix,
// where only the UTF-8 encoder can reject, which throws and is reported
// as a translation error; only after that is the combined ud-suffix
// required to start with an underscore. The token source is the
// space-joined member sources.
PostToken AnalyzeStringSequence(const std::vector<string>& sources);
