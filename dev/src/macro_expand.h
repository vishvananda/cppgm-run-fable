#pragma once

#include <deque>
#include <string>
#include <vector>

using std::deque;
using std::string;
using std::vector;

#include "macro_table.h"
#include "pp_token.h"

// Produces the dynamically computed token of a builtin predefined macro
// (__FILE__/__LINE__) for an invocation through `head`: the value is
// derived from head's file/line stamp and the owner's presumed-position
// state. Implemented by the PA5 Preprocessor.
struct IBuiltinTokenSource
{
	virtual PPToken MakeBuiltinToken(EMacroBuiltin builtin,
	                                 const PPToken& head) = 0;

	virtual ~IBuiltinTokenSource() {}
};

// Macro replacement (16.3) over one text-sequence, with the course-defined
// nesting semantics (see pa4/plan.md): every token carries a blacklist of
// macro names it can never invoke (an interned PaintSet, so shared paint
// is one allocation). Invoking macro M via head token T (and
// closing paren R for a function-like invocation) paints
//   - replacement-origin tokens (copies of the replacement list, stringize
//     and paste results) with base = (T ∩ R) ∪ {M} (object-like: T ∪ {M}),
//   - argument-origin tokens with their own paint ∪ T ∪ {M} (the full,
//     unintersected head paint -- the course deviation from C).
// A head naming a macro in its own blacklist is flagged noninvokable and
// emitted as a plain identifier.
//
// Throws std::runtime_error for invocation-time errors: unterminated
// argument lists, argument count mismatches, pastes that do not form a
// single preprocessing-token, and __VA_ARGS__ reaching a scan.
class MacroExpander
{
public:
	// `builtins` resolves builtin predefined macros; null (the PA4 input
	// class, which has none) makes invoking one a logic error.
	explicit MacroExpander(const MacroTable& table,
	                       IBuiltinTokenSource* builtins = 0)
		: table_(table), builtins_(builtins)
	{}

	// New-lines must already be folded into ws_before by the caller.
	// Arguments are expanded with the same entry point ("as if the
	// argument formed the rest of the file" -- but in isolation, so a
	// trailing function-like macro name stays unexpanded). The rvalue
	// form moves the sequence into the scan instead of copying it.
	vector<PPToken> ExpandTextSequence(const vector<PPToken>& tokens);
	vector<PPToken> ExpandTextSequence(vector<PPToken>&& tokens);

private:
	void Scan(deque<PPToken>& input, vector<PPToken>& output);
	// Consumes tokens after the already-popped lparen through the
	// matching rparen, which is returned through close.
	vector<vector<PPToken>> CollectArguments(deque<PPToken>& input,
	                                         const MacroDefinition& macro,
	                                         PPToken& close);
	vector<PPToken> Substitute(const MacroDefinition& macro,
	                           const vector<vector<PPToken>>& args,
	                           const PPToken& head, const PPToken& close);
	void PastePass(vector<PPToken>& items, const PaintSet& base_paint);
	PPToken Stringize(const vector<PPToken>& raw) const;
	PPToken RetokenizeSpelling(const string& spelling) const;

	const MacroTable& table_;
	IBuiltinTokenSource* builtins_;
	PaintInterner paints_;
};
