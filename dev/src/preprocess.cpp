#include "preprocess.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "ctrl_expr.h"
#include "numeric_literals.h"
#include "post_token.h"
#include "pp_tokenizer.h"
#include "source_translation.h"
#include "text_literals.h"

using std::runtime_error;

// bootstrap system call interface, used by GetPreprocFileId (PA5 starter
// kit; system calls proper are a later assignment)
extern "C" long int syscall(long int n, ...) throw ();

bool GetPreprocFileId(const string& path, PreprocFileId& out_fileid)
{
	struct
	{
		unsigned long int dev;
		unsigned long int ino;
		long int unused[16];
	} data;

	int res = syscall(4, path.c_str(), &data);

	out_fileid = std::make_pair(data.dev, data.ino);

	return res == 0;
}

namespace {

// Reference-pinned include nesting cap ("Maximum include nexting
// reached"): a chain of 198 nested includes succeeds and the 199th
// fails (bisected against preproc-ref).
const int kMaxIncludeDepth = 198;

// Phases 1-3 over an internal spelling (predefined macro replacement
// lists, destringized _Pragma contents); new-lines are dropped.
vector<PPToken> TokenizeSpelling(const string& text)
{
	TranslatedSource source = TranslateSource(text);
	PPTokenCollector collector;
	TokenizePPTokens(source, collector);
	vector<PPToken> tokens;
	for (size_t i = 0; i < collector.tokens.size(); i++)
	{
		if (collector.tokens[i].kind != PPT_NEW_LINE)
			tokens.push_back(collector.tokens[i]);
	}
	return tokens;
}

// The operand of `defined` and the name after #ifdef/#ifndef: an
// identifier, or an identifier-like operator (`and`, `or_eq`, ... --
// the reference folds `defined and` to 0). Alternative (digraph) and
// symbol operators never start with a letter.
bool IsMacroNameToken(const PPToken& token)
{
	if (token.kind == PPT_IDENTIFIER)
		return true;
	return token.kind == PPT_OP_OR_PUNC && !token.data.empty() &&
		((token.data[0] >= 'a' && token.data[0] <= 'z') ||
		 (token.data[0] >= 'A' && token.data[0] <= 'Z'));
}

// PPToken -> PostToken in the PA3 identifier_or_keyword context
// (identifiers are NOT folded to keywords; #/##/%:/%:%: have no phase-7
// mapping and stay invalid; strings, header-names, and stray characters
// are non-evaluable).
PostToken ConvertControllingToken(const PPToken& token)
{
	switch (token.kind)
	{
	case PPT_IDENTIFIER:
	{
		PostToken converted;
		converted.kind = PTK_IDENTIFIER;
		converted.source = token.data;
		return converted;
	}
	case PPT_PP_NUMBER:
		return AnalyzePPNumber(token.data);
	case PPT_CHARACTER_LITERAL:
	case PPT_UD_CHARACTER_LITERAL:
		return AnalyzeCharLiteral(token.data).token;
	case PPT_OP_OR_PUNC:
	{
		PostToken converted;
		converted.source = token.data;
		if (LookupSimpleTokenType(token.data, converted.token_type))
			converted.kind = PTK_SIMPLE;
		else
			converted.kind = PTK_INVALID;
		return converted;
	}
	default:
		return MakeInvalidToken(token.data);
	}
}

// 2.14.2: the integer-literal types an integer suffix/value can produce.
bool IsIntegerLiteral(const PostToken& token)
{
	if (token.kind != PTK_LITERAL)
		return false;
	switch (token.type)
	{
	case FT_INT:
	case FT_UNSIGNED_INT:
	case FT_LONG_INT:
	case FT_UNSIGNED_LONG_INT:
	case FT_LONG_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT:
		return true;
	default:
		return false;
	}
}

// Phase-7 value of one ordinary (narrow character) string literal, used
// by #include and #line operands; the trailing terminator is dropped.
string StringLiteralValue(const string& spelling)
{
	PostToken analyzed = AnalyzeStringSequence(vector<string>(1, spelling));
	if (analyzed.kind != PTK_LITERAL_ARRAY || analyzed.type != FT_CHAR)
		throw runtime_error("expected an ordinary string literal: " +
		                    spelling);
	return analyzed.data.substr(0, analyzed.data.size() - 1);
}

// 16.9: delete the encoding prefix and the delimiting quotes, reverse
// the \" and \\ escapes; a raw string contributes its raw content.
string DestringizePragma(const string& spelling)
{
	size_t quote = spelling.find('"');
	if (spelling.find('R') < quote)
	{
		size_t open = spelling.find('(', quote);
		size_t delim_len = open - (quote + 1);
		return spelling.substr(open + 1,
		                       spelling.size() - (open + 1) - (delim_len + 2));
	}
	size_t end = spelling.size() - 1;
	string out;
	for (size_t i = quote + 1; i < end; i++)
	{
		if (spelling[i] == '\\' && i + 1 < end &&
		    (spelling[i + 1] == '"' || spelling[i + 1] == '\\'))
			i++;
		out += spelling[i];
	}
	return out;
}

// String-literal spelling of a presumed file name (__FILE__ expansion).
string FileNameSpelling(const string& name)
{
	string spelling = "\"";
	for (size_t i = 0; i < name.size(); i++)
	{
		if (name[i] == '"' || name[i] == '\\')
			spelling += '\\';
		spelling += name[i];
	}
	spelling += '"';
	return spelling;
}

} // namespace

Preprocessor::Preprocessor(IPPTokenStream& output,
                           const vector<pair<string, string>>& object_macros)
	: expander_(table_, this), output_(output), current_file_(-1),
	  cond_base_(0), include_depth_(0)
{
	for (size_t i = 0; i < object_macros.size(); i++)
		DefineObjectMacro(object_macros[i].first, object_macros[i].second);
	table_.DefineBuiltin("__FILE__", kBuiltinFile);
	table_.DefineBuiltin("__LINE__", kBuiltinLine);
}

void Preprocessor::DefineObjectMacro(const string& name,
                                     const string& spelling)
{
	table_.Define(TokenizeSpelling(name + " " + spelling));
}

void Preprocessor::AddIncludeDir(const string& dir)
{
	include_dirs_.push_back(dir);
}

void Preprocessor::ProcessSourceFile(const string& path)
{
	std::ifstream in(path.c_str(), std::ios::binary);
	if (!in)
		throw runtime_error("cannot open source file: " + path);
	std::ostringstream buffer;
	buffer << in.rdbuf();
	ProcessSourceText(path, buffer.str());
}

void Preprocessor::ProcessSourceText(const string& presumed_name,
                                     const string& bytes)
{
	int file_index = (int)files_.size();
	FileInstance instance;
	instance.presumed_name = presumed_name;
	instance.line_offset = 0;
	files_.push_back(instance);
	ProcessFileTokens(TokenizeSource(bytes, file_index), file_index);
	output_.emit_eof();
}

vector<PPToken> Preprocessor::TokenizeSource(const string& bytes,
                                             int file_index)
{
	TranslatedSource source = TranslateSource(bytes);
	PPTokenCollector collector;
	TokenizePPTokens(source, collector, &collector);
	for (size_t i = 0; i < collector.tokens.size(); i++)
		collector.tokens[i].file = file_index;
	return std::move(collector.tokens);
}

bool Preprocessor::IsActive() const
{
	return conds_.empty() || conds_.back().active;
}

// The PA4 walk extended with conditional skipping: a `new-line #` starts
// a directive and its new-line ends it; every directive delimits the
// text-sequences around it. Inactive text is dropped without expansion.
// A file must close every conditional group it opened (16.1: the whole
// group lives in one file). Owns `tokens` so each one moves (not
// copies) into its directive line or text-sequence.
void Preprocessor::ProcessFileTokens(vector<PPToken> tokens, int file_index)
{
	int saved_file = current_file_;
	size_t saved_base = cond_base_;
	current_file_ = file_index;
	cond_base_ = conds_.size();

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
			FlushText(text);
			newline_ws = false;
			vector<PPToken> line;
			for (i++; i < tokens.size() && tokens[i].kind != PPT_NEW_LINE;
			     i++)
				line.push_back(std::move(tokens[i]));
			const PPToken& terminator =
				i < tokens.size() ? tokens[i]
				                  : (line.empty() ? token : line.back());
			ProcessDirective(line, terminator);
			continue;
		}
		line_start = false;
		if (IsActive())
		{
			text.push_back(std::move(tokens[i]));
			if (newline_ws)
			{
				text.back().ws_before = true;
				newline_ws = false;
			}
		}
		i++;
	}
	FlushText(text);

	if (conds_.size() != cond_base_)
		throw runtime_error("unterminated conditional directive at end of "
		                    "file: " + files_[file_index].presumed_name);
	current_file_ = saved_file;
	cond_base_ = saved_base;
}

void Preprocessor::ProcessDirective(const vector<PPToken>& line,
                                    const PPToken& terminator)
{
	if (line.empty())
		return; // 16p8: null directive (active or not)
	string name = line[0].kind == PPT_IDENTIFIER ? line[0].data : "";
	vector<PPToken> args(line.begin() + (name.empty() ? 0 : 1), line.end());
	// reference-pinned: a directive is processed once its terminating
	// new-line is consumed, so __LINE__ in its tokens reports the
	// directive's end line (visible when a splice continues the line)
	for (size_t i = 0; i < args.size(); i++)
		args[i].line = terminator.line;

	// conditional directives are recognized (and their group ordering
	// validated) even inside inactive sections
	if (name == "if")
		HandleIf(args);
	else if (name == "ifdef" || name == "ifndef")
		HandleIfdef(args, name == "ifndef");
	else if (name == "elif")
		HandleElif(args);
	else if (name == "else")
		HandleElse(args);
	else if (name == "endif")
		HandleEndif(args);
	else if (!IsActive())
		return; // everything else is skipped in an inactive section
	else if (name == "define")
		table_.Define(args);
	else if (name == "undef")
		table_.Undef(args);
	else if (name == "include")
		HandleInclude(args);
	else if (name == "line")
		HandleLine(args, terminator);
	else if (name == "error")
		throw runtime_error("#error directive in active section");
	else if (name == "pragma")
		HandlePragma(args);
	else
		throw runtime_error("non-directive # line in active section");
}

void Preprocessor::HandleIf(const vector<PPToken>& args)
{
	CondGroup group;
	group.parent_active = IsActive();
	group.taken = false;
	group.seen_else = false;
	group.active = false;
	if (group.parent_active)
	{
		bool value = EvaluateCondition(args);
		group.taken = value;
		group.active = value;
	}
	conds_.push_back(group);
}

void Preprocessor::HandleIfdef(const vector<PPToken>& args, bool negate)
{
	CondGroup group;
	group.parent_active = IsActive();
	group.taken = false;
	group.seen_else = false;
	group.active = false;
	if (group.parent_active)
	{
		bool value = LookupDefinedName(args) != negate;
		group.taken = value;
		group.active = value;
	}
	conds_.push_back(group);
}

// The name queried by #ifdef/#ifndef. Extra tokens after the name are
// ignored (reference behavior); a missing or non-name token is an error.
bool Preprocessor::LookupDefinedName(const vector<PPToken>& args) const
{
	if (args.empty())
		throw runtime_error("no tokens in #ifdef/#ifndef");
	if (!IsMacroNameToken(args[0]))
		throw runtime_error("expected identifier after #ifdef/#ifndef");
	return table_.Lookup(args[0].data) != 0;
}

void Preprocessor::HandleElif(const vector<PPToken>& args)
{
	if (conds_.size() <= cond_base_)
		throw runtime_error("#elif without #if");
	CondGroup& group = conds_.back();
	if (group.seen_else)
		throw runtime_error("#elif after #else");
	if (group.parent_active && !group.taken)
	{
		bool value = EvaluateCondition(args);
		group.taken = value;
		group.active = value;
	}
	else
	{
		group.active = false;
	}
}

void Preprocessor::HandleElse(const vector<PPToken>& args)
{
	if (conds_.size() <= cond_base_)
		throw runtime_error("#else without #if");
	CondGroup& group = conds_.back();
	if (group.seen_else)
		throw runtime_error("multiple #else in one #if group");
	// reference-pinned: extra tokens are an error only when this #else
	// becomes the active branch (ignored after a taken branch, unlike
	// #endif whose check needs only parent_active)
	if (group.parent_active && !group.taken && !args.empty())
		throw runtime_error("extra tokens after #else");
	group.seen_else = true;
	group.active = group.parent_active && !group.taken;
	group.taken = true;
}

void Preprocessor::HandleEndif(const vector<PPToken>& args)
{
	if (conds_.size() <= cond_base_)
		throw runtime_error("#endif without #if");
	if (conds_.back().parent_active && !args.empty())
		throw runtime_error("extra tokens after #endif");
	conds_.pop_back();
}

// 16.1: fold the defined operator, macro-replace what remains, evaluate
// with the PA3 calculator. An empty expression or an "error" outcome is
// a PA5 error.
bool Preprocessor::EvaluateCondition(const vector<PPToken>& tokens)
{
	if (tokens.empty())
		throw runtime_error("no tokens in controlling expression");
	vector<PPToken> expanded =
		expander_.ExpandTextSequence(FoldDefinedOperators(tokens));
	if (expanded.empty())
		throw runtime_error("no tokens in controlling expression");
	std::vector<PostToken> converted;
	for (size_t i = 0; i < expanded.size(); i++)
		converted.push_back(ConvertControllingToken(expanded[i]));
	const MacroTable& table = table_;
	CtrlExprResult result = EvaluateControllingResult(
		converted,
		[&table](const string& name) { return table.Lookup(name) != 0; });
	if (result.kind != CtrlExprResult::kValue)
		throw runtime_error("error in controlling expression");
	return result.value != 0;
}

// Folds each well-formed `defined name` / `defined ( name )` to a
// pp-number 1/0 before macro replacement, so operands are not expanded
// (16.1p4). Malformed defined operators are left for the calculator,
// whose pinned recovery always reports the line as an error -- the same
// PA5 outcome regardless of what expansion does to the leftovers.
vector<PPToken> Preprocessor::FoldDefinedOperators(
	const vector<PPToken>& tokens)
{
	vector<PPToken> out;
	size_t i = 0;
	while (i < tokens.size())
	{
		const PPToken& token = tokens[i];
		int name_at = 0;
		if (token.kind == PPT_IDENTIFIER && token.data == "defined")
		{
			if (i + 1 < tokens.size() && IsMacroNameToken(tokens[i + 1]))
				name_at = 1;
			else if (i + 3 < tokens.size() && IsOp(tokens[i + 1], "(") &&
			         IsMacroNameToken(tokens[i + 2]) &&
			         IsOp(tokens[i + 3], ")"))
				name_at = 2;
		}
		if (name_at == 0)
		{
			out.push_back(token);
			i++;
			continue;
		}
		bool defined = table_.Lookup(tokens[i + name_at].data) != 0;
		PPToken folded(PPT_PP_NUMBER, defined ? "1" : "0");
		folded.ws_before = token.ws_before;
		folded.file = token.file;
		folded.line = token.line;
		out.push_back(folded);
		i += name_at == 1 ? 2 : 4;
	}
	return out;
}

// 16.2 with the course-defined search: macro-replace the operand, accept
// one header-name or one ordinary string-literal, then try the presumed
// __FILE__'s directory followed by the current working directory. A
// fileid already pragma-onced skips the include.
void Preprocessor::HandleInclude(const vector<PPToken>& args)
{
	vector<PPToken> expanded = expander_.ExpandTextSequence(args);
	if (expanded.size() != 1)
		throw runtime_error("invalid tokens in #include");
	string nextf;
	if (expanded[0].kind == PPT_HEADER_NAME)
		nextf = expanded[0].data.substr(1, expanded[0].data.size() - 2);
	else if (expanded[0].kind == PPT_STRING_LITERAL)
		nextf = StringLiteralValue(expanded[0].data);
	else
		throw runtime_error("invalid token in #include: " +
		                    expanded[0].data);

	const string& presumed = files_[current_file_].presumed_name;
	size_t slash = presumed.rfind('/');
	string chosen;
	PreprocFileId fileid;
	bool found = false;
	if (slash != string::npos)
	{
		string pathrel = presumed.substr(0, slash + 1) + nextf;
		if (GetPreprocFileId(pathrel, fileid))
		{
			chosen = pathrel;
			found = true;
		}
	}
	// PA29: -I directories search between the including file's
	// directory and the working-directory fallback.
	for (size_t i = 0; !found && i < include_dirs_.size(); i++)
	{
		string dir = include_dirs_[i];
		if (!dir.empty() && dir[dir.size() - 1] != '/')
			dir += '/';
		if (GetPreprocFileId(dir + nextf, fileid))
		{
			chosen = dir + nextf;
			found = true;
		}
	}
	if (!found && GetPreprocFileId(nextf, fileid))
	{
		chosen = nextf;
		found = true;
	}
	if (!found)
		throw runtime_error("include file not found: " + nextf);
	if (pragma_onced_.count(fileid))
		return;

	if (include_depth_ >= kMaxIncludeDepth)
		throw runtime_error("maximum include nesting reached");
	std::ifstream in(chosen.c_str(), std::ios::binary);
	if (!in)
		throw runtime_error("cannot read include file: " + chosen);
	std::ostringstream buffer;
	buffer << in.rdbuf();
	int file_index = (int)files_.size();
	FileInstance instance;
	instance.presumed_name = chosen;
	instance.line_offset = 0;
	files_.push_back(instance);
	include_depth_++;
	ProcessFileTokens(TokenizeSource(buffer.str(), file_index), file_index);
	include_depth_--;
}

// 16.4: macro-replace, then `integer-literal` or `integer-literal
// string-literal`. The line after the directive's terminating new-line
// becomes presumed line N (the ref accepts any integer literal,
// including 0 and unsigned suffixes); the string rewrites the presumed
// __FILE__.
void Preprocessor::HandleLine(const vector<PPToken>& args,
                              const PPToken& terminator)
{
	vector<PPToken> expanded = expander_.ExpandTextSequence(args);
	if (expanded.empty() || expanded[0].kind != PPT_PP_NUMBER)
		throw runtime_error("expected integer after #line");
	PostToken number = AnalyzePPNumber(expanded[0].data);
	if (!IsIntegerLiteral(number))
		throw runtime_error("expected integer after #line");
	bool have_name = false;
	string name;
	if (expanded.size() > 1)
	{
		if (expanded[1].kind != PPT_STRING_LITERAL)
			throw runtime_error("expected string after integer in #line");
		if (expanded.size() > 2)
			throw runtime_error("extra tokens after #line");
		name = StringLiteralValue(expanded[1].data);
		have_name = true;
	}
	FileInstance& instance = files_[current_file_];
	instance.line_offset =
		(long)LittleEndianValue(number.data) - (terminator.line + 1);
	if (have_name)
		instance.presumed_name = name;
}

// 16.6: `once` (no other tokens) marks the current presumed __FILE__;
// every other pragma, including the course mock `cppgm_mock_unknown`
// and an empty pragma, is ignored.
void Preprocessor::HandlePragma(const vector<PPToken>& args)
{
	if (args.empty())
		return;
	if (args[0].kind == PPT_IDENTIFIER && args[0].data == "once")
	{
		if (args.size() > 1)
			throw runtime_error("extra tokens after #pragma once");
		PragmaOnceCurrentFile();
	}
}

void Preprocessor::PragmaOnceCurrentFile()
{
	// reference-pinned: a presumed __FILE__ that cannot be stat'ed (e.g.
	// rewritten by #line to a nonexistent name) is an error, not a no-op
	PreprocFileId fileid;
	if (!GetPreprocFileId(files_[current_file_].presumed_name, fileid))
		throw runtime_error("cannot stat #pragma once file: " +
		                    files_[current_file_].presumed_name);
	pragma_onced_.insert(fileid);
}

// Course-defined _Pragma operator: destringize and dispatch through the
// #pragma handler (so `_Pragma("once")` and `#pragma once` coincide).
void Preprocessor::ExecutePragmaOperator(const string& spelling)
{
	HandlePragma(TokenizeSpelling(DestringizePragma(spelling)));
}

// Macro-replaces one text-sequence and replays it into the phase 5-7
// stream, executing and removing _Pragma operator invocations (course
// rule: recognized only here, after all macro replacement, and each
// `_Pragma` must head `( string-literal )`).
void Preprocessor::FlushText(vector<PPToken>& text)
{
	if (text.empty())
		return;
	vector<PPToken> replaced = expander_.ExpandTextSequence(std::move(text));
	text.clear();
	size_t i = 0;
	while (i < replaced.size())
	{
		const PPToken& token = replaced[i];
		if (token.kind == PPT_IDENTIFIER && token.data == "_Pragma")
		{
			if (i + 3 >= replaced.size() || !IsOp(replaced[i + 1], "(") ||
			    replaced[i + 2].kind != PPT_STRING_LITERAL ||
			    !IsOp(replaced[i + 3], ")"))
				throw runtime_error("_Pragma must be followed by "
				                    "( string-literal )");
			ExecutePragmaOperator(replaced[i + 2].data);
			i += 4;
			continue;
		}
		EmitPPToken(token, output_);
		i++;
	}
}

PPToken Preprocessor::MakeBuiltinToken(EMacroBuiltin builtin,
                                       const PPToken& head)
{
	const FileInstance& instance =
		files_[head.file >= 0 ? head.file : current_file_];
	if (builtin == kBuiltinFile)
		return PPToken(PPT_STRING_LITERAL,
		               FileNameSpelling(instance.presumed_name));
	std::ostringstream line;
	line << head.line + instance.line_offset;
	return PPToken(PPT_PP_NUMBER, line.str());
}
