#pragma once

#include <set>
#include <string>
#include <vector>

using std::set;
using std::string;
using std::vector;

#include "IPPTokenStream.h"

// Value form of one phase-3 IPPTokenStream emission. Whitespace runs are
// folded into the next token's ws_before; new-lines stay real tokens
// because directive structure depends on them.
enum EPPTokenKind
{
	PPT_NEW_LINE,
	PPT_HEADER_NAME,
	PPT_IDENTIFIER,
	PPT_PP_NUMBER,
	PPT_CHARACTER_LITERAL,
	PPT_UD_CHARACTER_LITERAL,
	PPT_STRING_LITERAL,
	PPT_UD_STRING_LITERAL,
	PPT_OP_OR_PUNC,
	PPT_NON_WHITESPACE_CHAR,
	// Result of substituting an empty argument adjacent to ## (16.3.3).
	// Exists only between substitution and the paste pass.
	PPT_PLACEMARKER
};

// One preprocessing-token plus the state macro replacement attaches to it.
// `blacklist` is the blue paint: macro names this token can never invoke.
// `noninvokable` records that an invocation of this token was aborted, so
// it is never reconsidered (course rule; blacklists never shrink, so this
// is equivalent to re-checking, but it documents the intent).
// `paste_op` marks a ## that was written in a replacement list and
// therefore operates; ## tokens that arrive via substitution are inert.
struct PPToken
{
	PPToken()
		: kind(PPT_NEW_LINE), ws_before(false), noninvokable(false),
		  paste_op(false)
	{}

	PPToken(EPPTokenKind k, const string& spelling)
		: kind(k), data(spelling), ws_before(false), noninvokable(false),
		  paste_op(false)
	{}

	EPPTokenKind kind;
	string data;
	bool ws_before;
	set<string> blacklist;
	bool noninvokable;
	bool paste_op;
};

// Collects a phase-3 token stream into PPToken values. Whitespace
// sequences become ws_before on the following token (or are dropped at
// eof); the eof emission itself is not stored.
class PPTokenCollector : public IPPTokenStream
{
public:
	PPTokenCollector() : pending_ws_(false) {}

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

	vector<PPToken> tokens;

private:
	void Add(EPPTokenKind kind, const string& data);

	bool pending_ws_;
};

// Replays one token into a phase 5-7 consumer (whitespace flags are
// dropped; the PostTokenizer ignores spacing).
void EmitPPToken(const PPToken& token, IPPTokenStream& output);

// Digraph-aware operator identity (2.6: %: and %:%: behave as # and ##).
bool IsHash(const PPToken& token);
bool IsHashHash(const PPToken& token);
bool IsOp(const PPToken& token, const char* spelling);
