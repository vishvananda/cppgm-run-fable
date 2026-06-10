#include "macro_expand.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

#include "pp_tokenizer.h"
#include "source_translation.h"

using std::runtime_error;

namespace {

const char* const kVaArgs = "__VA_ARGS__";

// Parameter slot used by a replacement-list token: 0..params.size()-1 for
// named parameters, params.size() for __VA_ARGS__, -1 for everything else.
int ParamIndex(const MacroDefinition& macro, const PPToken& token)
{
	if (token.kind != PPT_IDENTIFIER)
		return -1;
	if (macro.variadic && token.data == kVaArgs)
		return (int)macro.params.size();
	for (size_t i = 0; i < macro.params.size(); i++)
	{
		if (macro.params[i] == token.data)
			return (int)i;
	}
	return -1;
}

set<string> IntersectPaint(const set<string>& a, const set<string>& b)
{
	set<string> result;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
	                      std::inserter(result, result.begin()));
	return result;
}

void AddPaint(PPToken& token, const set<string>& paint)
{
	token.blacklist.insert(paint.begin(), paint.end());
}

// 16.3.2p2: \ is inserted before the " and \ characters of character and
// string literal spellings only.
bool NeedsStringizeEscape(const PPToken& token)
{
	return token.kind == PPT_CHARACTER_LITERAL ||
		token.kind == PPT_UD_CHARACTER_LITERAL ||
		token.kind == PPT_STRING_LITERAL ||
		token.kind == PPT_UD_STRING_LITERAL;
}

} // namespace

vector<PPToken> MacroExpander::ExpandTextSequence(const vector<PPToken>& tokens)
{
	deque<PPToken> input(tokens.begin(), tokens.end());
	vector<PPToken> output;
	Scan(input, output);
	return output;
}

void MacroExpander::Scan(deque<PPToken>& input, vector<PPToken>& output)
{
	while (!input.empty())
	{
		PPToken head = input.front();
		input.pop_front();
		if (head.kind != PPT_IDENTIFIER)
		{
			output.push_back(head);
			continue;
		}
		if (head.data == kVaArgs)
			throw runtime_error("__VA_ARGS__ outside a variadic macro "
			                    "replacement list");
		const MacroDefinition* macro =
			head.noninvokable ? 0 : table_.Lookup(head.data);
		if (!macro ||
		    (macro->function_like &&
		     (input.empty() || !IsOp(input.front(), "("))))
		{
			output.push_back(head);
			continue;
		}
		if (head.blacklist.count(head.data))
		{
			head.noninvokable = true;
			output.push_back(head);
			continue;
		}
		vector<vector<PPToken>> args;
		PPToken close;
		if (macro->function_like)
		{
			input.pop_front();
			args = CollectArguments(input, *macro, close);
		}
		vector<PPToken> replaced = Substitute(*macro, args, head, close);
		input.insert(input.begin(), replaced.begin(), replaced.end());
	}
}

vector<vector<PPToken>> MacroExpander::CollectArguments(
	deque<PPToken>& input, const MacroDefinition& macro, PPToken& close)
{
	vector<vector<PPToken>> args;
	vector<PPToken> current;
	size_t named = macro.params.size();
	int depth = 0;
	while (!input.empty())
	{
		PPToken token = input.front();
		input.pop_front();
		if (depth == 0 && IsOp(token, ")"))
		{
			args.push_back(current);
			close = token;
			// () invokes a zero-parameter macro with no arguments. All
			// other empty spans between delimiters are empty arguments.
			if (named == 0 && !macro.variadic && args.size() == 1 &&
			    args[0].empty())
				args.clear();
			if (macro.variadic)
			{
				// 16.3p4: more arguments than named parameters
				if (args.size() <= named)
					throw runtime_error("too few arguments to variadic "
					                    "macro " + macro.name);
			}
			else if (args.size() != named)
			{
				throw runtime_error("wrong number of arguments to macro " +
				                    macro.name);
			}
			return args;
		}
		if (depth == 0 && IsOp(token, ",") &&
		    (!macro.variadic || args.size() < named))
		{
			args.push_back(current);
			current.clear();
			continue;
		}
		if (IsOp(token, "("))
			depth++;
		else if (IsOp(token, ")"))
			depth--;
		current.push_back(token);
	}
	throw runtime_error("unterminated invocation of macro " + macro.name);
}

vector<PPToken> MacroExpander::Substitute(const MacroDefinition& macro,
                                          const vector<vector<PPToken>>& args,
                                          const PPToken& head,
                                          const PPToken& close)
{
	set<string> base = head.blacklist;
	if (macro.function_like)
		base = IntersectPaint(base, close.blacklist);
	base.insert(macro.name);
	// Argument-origin tokens take the full head paint (course rule), not
	// the intersected base, and keep what argument expansion added.
	set<string> arg_paint = head.blacklist;
	arg_paint.insert(macro.name);

	vector<vector<PPToken>> expanded(args.size());
	vector<bool> expanded_done(args.size(), false);

	const vector<PPToken>& list = macro.replacement;
	vector<PPToken> items;
	for (size_t i = 0; i < list.size(); i++)
	{
		const PPToken& token = list[i];
		if (macro.function_like && IsHash(token))
		{
			// definition validation guarantees list[i + 1] is a parameter
			PPToken text = Stringize(args[ParamIndex(macro, list[i + 1])]);
			text.ws_before = token.ws_before;
			text.blacklist = base;
			items.push_back(text);
			i++;
			continue;
		}
		if (token.paste_op)
		{
			items.push_back(token);
			continue;
		}
		int param = macro.function_like ? ParamIndex(macro, token) : -1;
		if (param < 0)
		{
			PPToken copy = token;
			copy.blacklist = base;
			items.push_back(copy);
			continue;
		}
		bool next_to_paste = (!items.empty() && items.back().paste_op) ||
			(i + 1 < list.size() && list[i + 1].paste_op);
		const vector<PPToken>* source;
		if (next_to_paste)
		{
			source = &args[param];
		}
		else
		{
			if (!expanded_done[param])
			{
				expanded[param] = ExpandTextSequence(args[param]);
				expanded_done[param] = true;
			}
			source = &expanded[param];
		}
		if (source->empty())
		{
			if (next_to_paste)
			{
				PPToken marker(PPT_PLACEMARKER, "");
				marker.ws_before = token.ws_before;
				items.push_back(marker);
			}
			continue;
		}
		size_t first = items.size();
		for (size_t j = 0; j < source->size(); j++)
		{
			PPToken copy = (*source)[j];
			AddPaint(copy, arg_paint);
			items.push_back(copy);
		}
		items[first].ws_before = token.ws_before;
	}

	PastePass(items, base);
	// the result occupies the invocation's position in the stream
	if (!items.empty())
		items[0].ws_before = head.ws_before;
	return items;
}

void MacroExpander::PastePass(vector<PPToken>& items,
                              const set<string>& base_paint)
{
	vector<PPToken> result;
	size_t i = 0;
	while (i < items.size())
	{
		if (!items[i].paste_op)
		{
			result.push_back(items[i]);
			i++;
			continue;
		}
		// definition validation: ## is never first or last in the
		// replacement list, so both operands exist
		PPToken left = result.back();
		result.pop_back();
		PPToken right = items[i + 1];
		right.paste_op = false;
		i += 2;
		if (right.kind == PPT_PLACEMARKER)
		{
			// covers placemarker ## placemarker -> placemarker
			result.push_back(left);
		}
		else if (left.kind == PPT_PLACEMARKER)
		{
			right.ws_before = left.ws_before;
			result.push_back(right);
		}
		else
		{
			PPToken pasted = RetokenizeSpelling(left.data + right.data);
			pasted.ws_before = left.ws_before;
			pasted.blacklist = base_paint;
			result.push_back(pasted);
		}
	}
	items.clear();
	for (size_t j = 0; j < result.size(); j++)
	{
		if (result[j].kind != PPT_PLACEMARKER)
			items.push_back(result[j]);
	}
}

PPToken MacroExpander::Stringize(const vector<PPToken>& raw) const
{
	string body;
	for (size_t i = 0; i < raw.size(); i++)
	{
		if (i > 0 && raw[i].ws_before)
			body += ' ';
		if (!NeedsStringizeEscape(raw[i]))
		{
			body += raw[i].data;
			continue;
		}
		for (size_t j = 0; j < raw[i].data.size(); j++)
		{
			char c = raw[i].data[j];
			if (c == '"' || c == '\\')
				body += '\\';
			body += c;
		}
	}
	return PPToken(PPT_STRING_LITERAL, "\"" + body + "\"");
}

PPToken MacroExpander::RetokenizeSpelling(const string& spelling) const
{
	TranslatedSource source = TranslateSource(spelling);
	PPTokenCollector collector;
	TokenizePPTokens(source, collector);
	vector<PPToken> tokens;
	for (size_t i = 0; i < collector.tokens.size(); i++)
	{
		if (collector.tokens[i].kind != PPT_NEW_LINE)
			tokens.push_back(collector.tokens[i]);
	}
	if (tokens.size() != 1)
		throw runtime_error("pasting \"" + spelling + "\" does not produce "
		                    "a single preprocessing token");
	return tokens[0];
}
