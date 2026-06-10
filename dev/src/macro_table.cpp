#include "macro_table.h"

#include <algorithm>
#include <stdexcept>

using std::runtime_error;

namespace {

const char* const kVaArgs = "__VA_ARGS__";

bool IsParameter(const MacroDefinition& macro, const PPToken& token)
{
	if (token.kind != PPT_IDENTIFIER)
		return false;
	if (macro.variadic && token.data == kVaArgs)
		return true;
	return std::find(macro.params.begin(), macro.params.end(), token.data)
		!= macro.params.end();
}

// Splits `( identifier-list )` starting at line[pos] (the lparen) and
// returns the index just past the rparen.
size_t ParseParameterList(const vector<PPToken>& line, size_t pos,
                          MacroDefinition& macro)
{
	pos++;
	bool expect_param = true;
	for (; pos < line.size(); pos++)
	{
		const PPToken& token = line[pos];
		if (IsOp(token, ")"))
		{
			if (expect_param && !macro.params.empty())
				throw runtime_error("macro parameter missing before )");
			return pos + 1;
		}
		if (expect_param)
		{
			if (IsOp(token, "..."))
			{
				macro.variadic = true;
				if (pos + 1 >= line.size() || !IsOp(line[pos + 1], ")"))
					throw runtime_error("expected ) after ... in macro "
					                    "parameter list");
				return pos + 2;
			}
			if (token.kind != PPT_IDENTIFIER)
				throw runtime_error("expected parameter name in macro "
				                    "parameter list");
			if (token.data == kVaArgs)
				throw runtime_error("__VA_ARGS__ cannot be a macro "
				                    "parameter name");
			if (std::find(macro.params.begin(), macro.params.end(),
			              token.data) != macro.params.end())
				throw runtime_error("duplicate macro parameter name: " +
				                    token.data);
			macro.params.push_back(token.data);
			expect_param = false;
		}
		else
		{
			if (!IsOp(token, ","))
				throw runtime_error("expected , or ) in macro parameter "
				                    "list");
			expect_param = true;
		}
	}
	throw runtime_error("unterminated macro parameter list");
}

void ValidateReplacementList(MacroDefinition& macro)
{
	vector<PPToken>& list = macro.replacement;
	for (size_t i = 0; i < list.size(); i++)
	{
		if (IsHashHash(list[i]))
		{
			if (i == 0 || i + 1 == list.size())
				throw runtime_error("## cannot appear at either end of a "
				                    "replacement list");
			list[i].paste_op = true;
		}
		else if (IsHash(list[i]) && macro.function_like)
		{
			if (i + 1 == list.size() || !IsParameter(macro, list[i + 1]))
				throw runtime_error("# must be followed by a macro "
				                    "parameter");
		}
		else if (list[i].kind == PPT_IDENTIFIER && list[i].data == kVaArgs &&
		         !macro.variadic)
		{
			throw runtime_error("__VA_ARGS__ outside a variadic macro "
			                    "replacement list");
		}
	}
}

bool SameReplacementList(const vector<PPToken>& a, const vector<PPToken>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i].kind != b[i].kind || a[i].data != b[i].data)
			return false;
		// 16.3p1: white-space separation between tokens is part of the
		// identity; leading separation was trimmed away from position 0.
		if (i > 0 && a[i].ws_before != b[i].ws_before)
			return false;
	}
	return true;
}

bool SameDefinition(const MacroDefinition& a, const MacroDefinition& b)
{
	return a.function_like == b.function_like && a.variadic == b.variadic &&
		a.params == b.params &&
		SameReplacementList(a.replacement, b.replacement);
}

} // namespace

void MacroTable::Define(const vector<PPToken>& line)
{
	if (line.empty() || line[0].kind != PPT_IDENTIFIER)
		throw runtime_error("macro name missing in #define");
	MacroDefinition macro;
	macro.name = line[0].data;
	if (macro.name == kVaArgs)
		throw runtime_error("__VA_ARGS__ cannot be a macro name");

	size_t pos = 1;
	if (pos < line.size() && IsOp(line[pos], "(") && !line[pos].ws_before)
	{
		macro.function_like = true;
		pos = ParseParameterList(line, pos, macro);
	}
	else if (pos < line.size() && !line[pos].ws_before)
	{
		throw runtime_error("missing whitespace after object-like macro "
		                    "name");
	}

	macro.replacement.assign(line.begin() + pos, line.end());
	if (!macro.replacement.empty())
		macro.replacement[0].ws_before = false;
	ValidateReplacementList(macro);

	map<string, MacroDefinition>::iterator existing =
		macros_.find(macro.name);
	if (existing != macros_.end() && !SameDefinition(existing->second, macro))
		throw runtime_error("macro redefined with a different definition: " +
		                    macro.name);
	macros_[macro.name] = macro;
}

void MacroTable::Undef(const vector<PPToken>& line)
{
	if (line.empty() || line[0].kind != PPT_IDENTIFIER)
		throw runtime_error("macro name missing in #undef");
	if (line[0].data == kVaArgs)
		throw runtime_error("__VA_ARGS__ cannot be a macro name");
	if (line.size() > 1)
		throw runtime_error("extra tokens after #undef name");
	macros_.erase(line[0].data);
}

const MacroDefinition* MacroTable::Lookup(const string& name) const
{
	map<string, MacroDefinition>::const_iterator it = macros_.find(name);
	return it == macros_.end() ? 0 : &it->second;
}
