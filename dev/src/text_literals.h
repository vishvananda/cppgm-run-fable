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
PostToken AnalyzeCharLiteral(const string& source);

// Phases 6 and 7 for a maximal sequence of adjacent string-literal and
// user-defined-string-literal preprocessing-tokens (2.14.5, 2.14.8):
// decodes each body (raw bodies literally), applies the course
// concatenation rules (at most one distinct encoding-prefix and at most
// one distinct ud-suffix), appends the terminating 0 code point, and
// encodes the array as UTF-8/16/32 per the effective prefix. String
// code points are not range-checked (lone surrogates encode as-is);
// only the UTF-8 encoder can reject, which throws and is reported as a
// translation error. The token source is the space-joined member
// sources.
PostToken AnalyzeStringSequence(const std::vector<string>& sources);
