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
// mock name categories are spelling-derived facts stamped once per
// token at build time; the parser queries them as flags in the grammar
// positions that use them.
enum EParseTokenKind
{
	PTOK_SIMPLE,      // keyword or operator/punctuation (ETokenType)
	PTOK_IDENTIFIER,  // TT_IDENTIFIER
	PTOK_LITERAL,     // TT_LITERAL
	PTOK_RSHIFT_1,    // ST_RSHIFT_1 (first half of a split OP_RSHIFT)
	PTOK_RSHIFT_2,    // ST_RSHIFT_2 (second half)
	PTOK_EOF          // ST_EOF
};

enum EParseTokenFlags
{
	// mock name lookup categories (PTOK_IDENTIFIER)
	PTF_CLASS_NAME = 1 << 0,      // contains 'C'
	PTF_TEMPLATE_NAME = 1 << 1,   // contains 'T'
	PTF_TYPEDEF_NAME = 1 << 2,    // contains 'Y'
	PTF_ENUM_NAME = 1 << 3,       // contains 'E'
	PTF_NAMESPACE_NAME = 1 << 4,  // contains 'N'
	// context-sensitive keywords (PTOK_IDENTIFIER)
	PTF_ST_OVERRIDE = 1 << 5,     // spelled `override`
	PTF_ST_FINAL = 1 << 6,        // spelled `final`
	// special literals (PTOK_LITERAL)
	PTF_ST_EMPTYSTR = 1 << 7,     // spelled `""`
	PTF_ST_ZERO = 1 << 8          // spelled `0`
};

struct ParseToken
{
	ParseToken(EParseTokenKind kind_in, ETokenType simple_type_in,
	           const string& spelling_in, unsigned flags_in)
		: kind(kind_in), simple_type(simple_type_in), flags(flags_in),
		  spelling(spelling_in)
	{}

	bool HasFlag(unsigned mask) const
	{
		return (flags & mask) != 0;
	}

	EParseTokenKind kind;
	ETokenType simple_type;  // meaningful for PTOK_SIMPLE only
	unsigned flags;          // EParseTokenFlags facts of the spelling
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
