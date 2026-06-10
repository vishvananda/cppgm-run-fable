#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

// PA13 LowIR tokenizer. LowIR is mostly free-form between tokens; the
// lexer records line numbers because one grammar decision (the optional
// `return` value) is line-sensitive. Inputs from all translation units
// are concatenated before lexing.

enum ELowIRTokenKind
{
	LOWIR_TOK_IDENTIFIER,   // keywords, opcodes, metadata words
	LOWIR_TOK_GLOBAL_NAME,  // @name (globals and functions)
	LOWIR_TOK_TEMP_NAME,    // %name
	LOWIR_TOK_SLOT_NAME,    // $name
	LOWIR_TOK_BLOCK_NAME,   // ^name
	LOWIR_TOK_NUMBER,       // 42, 0x10, 1.5f, 2.25, 1.25L, 16x8
	LOWIR_TOK_PUNCT,        // : , = ( ) [ ] { } < > + - and ->
	LOWIR_TOK_DEBUG_LOC,    // !dbg(file, line, column)
	LOWIR_TOK_EOF
};

struct LowIRToken
{
	ELowIRTokenKind kind = LOWIR_TOK_EOF;
	string text;            // name tokens drop their sigil
	long line = 0;

	// Debug-location payload (kind == LOWIR_TOK_DEBUG_LOC).
	string dbg_file;
	long dbg_line = 0;
	long dbg_column = 0;
};

// Tokenizes `text`; throws std::runtime_error on stray characters or
// malformed `!dbg(...)` suffixes. Always ends with a LOWIR_TOK_EOF token.
vector<LowIRToken> LexLowIR(const string & text);
