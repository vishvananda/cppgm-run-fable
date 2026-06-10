#include "macro_preprocess.h"

#include <stdexcept>

using std::runtime_error;

void MacroPreprocessor::ProcessFile(const vector<PPToken>& tokens)
{
	vector<PPToken> text;
	bool line_start = true;
	bool newline_ws = false;
	size_t i = 0;
	while (i < tokens.size())
	{
		const PPToken& token = tokens[i];
		if (token.kind == PPT_NEW_LINE)
		{
			line_start = true;
			newline_ws = true;
			i++;
			continue;
		}
		if (line_start && IsHash(token))
		{
			FlushTextSequence(text);
			newline_ws = false;
			vector<PPToken> line;
			for (i++; i < tokens.size() && tokens[i].kind != PPT_NEW_LINE;
			     i++)
				line.push_back(tokens[i]);
			ProcessDirective(line);
			continue;
		}
		line_start = false;
		text.push_back(token);
		if (newline_ws)
		{
			text.back().ws_before = true;
			newline_ws = false;
		}
		i++;
	}
	FlushTextSequence(text);
	output_.emit_eof();
}

void MacroPreprocessor::ProcessDirective(const vector<PPToken>& line)
{
	if (line.empty())
		return; // 16p8: null directive
	if (line[0].kind == PPT_IDENTIFIER && line[0].data == "define")
		table_.Define(vector<PPToken>(line.begin() + 1, line.end()));
	else if (line[0].kind == PPT_IDENTIFIER && line[0].data == "undef")
		table_.Undef(vector<PPToken>(line.begin() + 1, line.end()));
	else
		throw runtime_error("unsupported preprocessing directive");
}

void MacroPreprocessor::FlushTextSequence(vector<PPToken>& text)
{
	if (text.empty())
		return;
	vector<PPToken> replaced = expander_.ExpandTextSequence(text);
	for (size_t i = 0; i < replaced.size(); i++)
		EmitPPToken(replaced[i], output_);
	text.clear();
}
