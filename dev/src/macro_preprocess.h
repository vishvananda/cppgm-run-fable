#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "IPPTokenStream.h"
#include "macro_expand.h"
#include "macro_table.h"
#include "pp_token.h"

// Translation phase 4 for the PA4 input class: `#define` and `#undef`
// directive lines (`#` first on a logical line) update the macro table;
// each maximal text-sequence between directives is macro-replaced with
// the table as of that point and replayed into the output stream (the
// PA2 PostTokenizer). One eof is emitted at the end of the file, so
// phase-6 string concatenation spans directive boundaries. New-lines
// inside a text-sequence become ordinary whitespace, so a function-like
// invocation may span source lines but never a directive. PA5 extends
// this walk with the remaining directives.
class MacroPreprocessor
{
public:
	explicit MacroPreprocessor(IPPTokenStream& output)
		: expander_(table_), output_(output)
	{}

	void ProcessFile(const vector<PPToken>& tokens);

private:
	void ProcessDirective(const vector<PPToken>& line);
	void FlushTextSequence(vector<PPToken>& text);

	MacroTable table_;
	MacroExpander expander_;
	IPPTokenStream& output_;
};
