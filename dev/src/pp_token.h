#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using std::map;
using std::pair;
using std::shared_ptr;
using std::string;
using std::vector;

#include "IPPTokenStream.h"

// Blue paint: the set of macro names a token can never invoke, stored
// as an immutable sorted name vector shared by pointer (null is the
// empty set). PaintInterner hash-conses the sets: however many tokens
// carry the same paint there is one allocation, and copying a token
// copies one pointer. Paint only ever changes through the interner,
// which keeps the sorted-unique invariant PaintContains relies on.
typedef shared_ptr<const vector<string>> PaintSet;

bool PaintContains(const PaintSet& paint, const string& name);

class PaintInterner
{
public:
	// Every operation returns an interned set: value-equal results are
	// pointer-equal, and subset/superset fast paths reuse the inputs.
	// Results are also memoized by input pointer identity (interned sets
	// live as long as the interner, so pointers stay unique), which makes
	// the per-invocation derivations cheap on hot expansion chains.
	PaintSet Insert(const PaintSet& paint, const string& name);
	PaintSet Union(const PaintSet& a, const PaintSet& b);
	PaintSet Intersect(const PaintSet& a, const PaintSet& b);

private:
	typedef const vector<string>* PaintKey;

	PaintSet Intern(vector<string>& names);

	map<vector<string>, PaintSet> sets_;
	map<pair<PaintKey, string>, PaintSet> insert_memo_;
	map<pair<PaintKey, PaintKey>, PaintSet> union_memo_;
	map<pair<PaintKey, PaintKey>, PaintSet> intersect_memo_;
};

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
// `paste_op` and `param_index` are definition-time facts stamped onto
// replacement-list tokens by MacroTable: paste_op marks a ## that was
// written in a replacement list and therefore operates (## tokens that
// arrive via substitution are inert), param_index is the parameter slot
// the token substitutes (0..params-1 named, params for __VA_ARGS__, -1
// for non-parameters).
// `file` and `line` are the PA5 position stamp: the 1-based physical
// source line of the token's first character and the index of its file
// instance in the owning Preprocessor's table (-1 outside PA5). Macro
// invocation re-stamps produced tokens with the head's position, so an
// invoked __FILE__/__LINE__ reports its own presumed position.
struct PPToken
{
	PPToken()
		: kind(PPT_NEW_LINE), ws_before(false), noninvokable(false),
		  paste_op(false), param_index(-1), file(-1), line(0)
	{}

	PPToken(EPPTokenKind k, const string& spelling)
		: kind(k), data(spelling), ws_before(false), noninvokable(false),
		  paste_op(false), param_index(-1), file(-1), line(0)
	{}

	EPPTokenKind kind;
	string data;
	bool ws_before;
	PaintSet blacklist;
	bool noninvokable;
	bool paste_op;
	int param_index;
	int file;
	long line;
};

// Collects a phase-3 token stream into PPToken values. Whitespace
// sequences become ws_before on the following token (or are dropped at
// eof); the eof emission itself is not stored. Passing the collector as
// the tokenizer's line sink stamps each token's physical line.
class PPTokenCollector : public IPPTokenStream, public IPPTokenLineSink
{
public:
	PPTokenCollector() : pending_ws_(false), pending_line_(0) {}

	void token_start_line(long line);

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
	long pending_line_;
};

// Replays one token into a phase 5-7 consumer (whitespace flags are
// dropped; the PostTokenizer ignores spacing).
void EmitPPToken(const PPToken& token, IPPTokenStream& output);

// Digraph-aware operator identity (2.6: %: and %:%: behave as # and ##).
bool IsHash(const PPToken& token);
bool IsHashHash(const PPToken& token);
bool IsOp(const PPToken& token, const char* spelling);
