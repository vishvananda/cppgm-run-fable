#pragma once

#include <string>

using std::string;

#include "post_token.h"

// Phase 7 analysis of a pp-number: classifies it as an integer-literal,
// floating-literal, user-defined-integer-literal, or
// user-defined-floating-literal (2.14.2, 2.14.4, 2.14.8) and computes the
// typed value. Integer literals are range-checked against the maximal
// type their suffix allows (Table 6, LP64 ABI); user-defined numeric
// literals keep their unscanned prefix spelling. Returns an invalid token
// when the spelling matches none of the grammars or the value does not
// fit.
PostToken AnalyzePPNumber(const string& source);
