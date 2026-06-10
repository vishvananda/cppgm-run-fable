#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "post_token.h"

// PA6 terminal vocabulary (pa6.gram): the phase-7 token sequence with
// every OP_RSHIFT split into the consecutive ST_RSHIFT_1 ST_RSHIFT_2
// pair (14.2.3) and the sequence terminated by one ST_EOF. All literal
// kinds (including user-defined) present one TT_LITERAL terminal. The
// special tokens ST_EMPTYSTR / ST_ZERO / ST_OVERRIDE / ST_FINAL and the
// mock name categories are spelling predicates on the carried source
// text, queried only in the grammar positions that use them.
enum EParseTokenKind
{
	PTOK_SIMPLE,      // keyword or operator/punctuation (ETokenType)
	PTOK_IDENTIFIER,  // TT_IDENTIFIER
	PTOK_LITERAL,     // TT_LITERAL
	PTOK_RSHIFT_1,    // ST_RSHIFT_1 (first half of a split OP_RSHIFT)
	PTOK_RSHIFT_2,    // ST_RSHIFT_2 (second half)
	PTOK_EOF          // ST_EOF
};

struct ParseToken
{
	ParseToken(EParseTokenKind kind_in, ETokenType simple_type_in,
	           const string& spelling_in)
		: kind(kind_in), simple_type(simple_type_in), spelling(spelling_in)
	{}

	EParseTokenKind kind;
	ETokenType simple_type;  // meaningful for PTOK_SIMPLE only
	string spelling;         // source text (PostToken::source)
};

// Converts the collected phase-7 tokens of one translation unit into
// the PA6 terminal sequence (ST_EOF guaranteed last). Throws
// std::runtime_error on a phase-7 invalid token: the srcfile is BAD.
vector<ParseToken> BuildParseTokens(const vector<PostToken>& tokens);

// PA6 mock name lookup (per the handout: category from the lexical
// form of the identifier).
bool IsMockClassName(const string& identifier);      // contains 'C'
bool IsMockTemplateName(const string& identifier);   // contains 'T'
bool IsMockTypedefName(const string& identifier);    // contains 'Y'
bool IsMockEnumName(const string& identifier);       // contains 'E'
bool IsMockNamespaceName(const string& identifier);  // contains 'N'
