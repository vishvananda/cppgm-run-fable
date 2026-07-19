#include "preprocess.h"

#include <iostream>
#include <stdexcept>

#include "hosted_probes.h"
#include "pp_tokenizer.h"
#include "source_translation.h"
#include "text_literals.h"

using std::runtime_error;

// PA34 hosted preprocessor surface: the __has_* probe operators, the
// driver macro/include controls, #warning, and -include processing.
// The core translation walk stays in preprocess.cpp; everything here is
// reached only through the hosted driver configuration.

namespace {

const char* const kHostedProbeNames[] = {
	"__has_include",
	"__has_include_next",
	"__has_builtin",
	"__has_feature",
	"__has_extension",
	"__has_attribute",
	"__has_cpp_attribute",
	"__building_module",
	"__is_identifier",
	0
};

bool IsHostedProbeNameImpl(const string& name)
{
	for (const char* const* probe = kHostedProbeNames; *probe; probe++)
	{
		if (name == *probe)
			return true;
	}
	return false;
}

// Phases 1-3 over an internal spelling (driver -D/-U bodies and host
// probe #define bodies); new-lines are dropped.
vector<PPToken> TokenizeMacroBody(const string& text)
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

// Phase-7 value of one ordinary string literal (quoted __has_include
// operand); the trailing terminator is dropped.
string ProbeStringValue(const string& spelling)
{
	PostToken analyzed = AnalyzeStringSequence(vector<string>(1, spelling));
	if (analyzed.kind != PTK_LITERAL_ARRAY || analyzed.type != FT_CHAR)
		throw runtime_error("expected an ordinary string literal: " +
		                    spelling);
	return analyzed.data.substr(0, analyzed.data.size() - 1);
}

}  // namespace

bool Preprocessor::IsHostedProbeName(const string& name)
{
	return IsHostedProbeNameImpl(name);
}

void Preprocessor::EnableHostedMode()
{
	if (hosted_)
		return;
	hosted_ = true;
	for (const char* const* probe = kHostedProbeNames; *probe; probe++)
		table_.DefineBuiltin(*probe, kBuiltinHostedProbe);
	// The source-location builtin operators expand at their spelling
	// site (the call-site re-evaluation of defaulted arguments is a
	// PA35 source_location boundary; columns are not tracked).
	table_.Define(TokenizeMacroBody("__builtin_FILE() __FILE__"));
	table_.Define(TokenizeMacroBody("__builtin_LINE() __LINE__"));
	table_.Define(TokenizeMacroBody("__builtin_COLUMN() 0"));
}

void Preprocessor::AddSystemIncludeDir(const string& dir)
{
	system_include_dirs_.push_back(dir);
}

void Preprocessor::AddPreIncludeFile(const string& path)
{
	pre_include_files_.push_back(path);
}

void Preprocessor::DefineHostMacroLine(const string& line)
{
	vector<PPToken> tokens = TokenizeMacroBody(line);
	if (tokens.empty() || tokens[0].kind != PPT_IDENTIFIER)
		return;
	if (table_.Lookup(tokens[0].data))
		return;
	table_.Define(tokens);
}

void Preprocessor::DefineCommandLineMacro(const string& spec)
{
	string body = spec;
	size_t equals = body.find('=');
	if (equals == string::npos)
		body += " 1";
	else
		body[equals] = ' ';
	vector<PPToken> tokens = TokenizeMacroBody(body);
	if (tokens.empty() || tokens[0].kind != PPT_IDENTIFIER)
		throw runtime_error("invalid macro definition: " + spec);
	table_.Undef(vector<PPToken>(1, tokens[0]));
	table_.Define(tokens);
}

void Preprocessor::UndefCommandLineMacro(const string& name)
{
	vector<PPToken> tokens = TokenizeMacroBody(name);
	if (tokens.size() != 1 || tokens[0].kind != PPT_IDENTIFIER)
		throw runtime_error("invalid macro name: " + name);
	table_.Undef(tokens);
}

// The driver's -include files, processed inside the main source file's
// stream before its first token. The search is the plain quoted search
// minus the including-file position (the main file has not contributed
// tokens yet): -I chain, working directory, system chain.
void Preprocessor::ProcessPreIncludeFiles()
{
	for (size_t i = 0; i < pre_include_files_.size(); i++)
	{
		IncludeResolution resolved =
			ResolveIncludeFile(pre_include_files_[i], -1);
		if (!resolved.found)
			throw runtime_error("cannot open -include file: " +
			                    pre_include_files_[i]);
		if (pragma_onced_.count(resolved.fileid))
			continue;
		ProcessIncludedFile(resolved);
	}
}

// #warning in an active section: report and continue (the hosted vendor
// headers use it for advisory notes).
void Preprocessor::HandleWarning(const vector<PPToken>& args,
                                 const PPToken& terminator)
{
	const FileInstance& instance = files_[current_file_];
	std::cerr << instance.presumed_name << ":"
	          << (terminator.line + instance.line_offset)
	          << ": warning: #warning";
	for (size_t i = 0; i < args.size(); i++)
		std::cerr << " " << args[i].data;
	std::cerr << std::endl;
}

// Folds each hosted probe invocation `__has_*( operand )` to a 1/0
// pp-number before `defined` folding and macro expansion, so operands
// are never expanded (mirroring the defined operator).
vector<PPToken> Preprocessor::FoldHostedProbeOperators(
	const vector<PPToken>& tokens)
{
	vector<PPToken> out;
	size_t i = 0;
	while (i < tokens.size())
	{
		const PPToken& token = tokens[i];
		// A probe name as the operand of `defined` (with or without
		// parens, as in `#elif defined __has_feature`) stays for the
		// defined folding: the operator names register as builtin
		// macros, so the existence query answers there.
		if (token.kind == PPT_IDENTIFIER && token.data == "defined")
		{
			out.push_back(token);
			i++;
			size_t skip = i;
			if (skip < tokens.size() && IsOp(tokens[skip], "("))
				skip++;
			if (skip < tokens.size() &&
			    tokens[skip].kind == PPT_IDENTIFIER)
				skip++;
			if (skip < tokens.size() && i < tokens.size() &&
			    IsOp(tokens[i], "(") && IsOp(tokens[skip], ")"))
				skip++;
			for (; i < skip; i++)
				out.push_back(tokens[i]);
			continue;
		}
		if (token.kind != PPT_IDENTIFIER ||
		    !IsHostedProbeNameImpl(token.data))
		{
			out.push_back(token);
			i++;
			continue;
		}
		size_t consumed = 0;
		long value = FoldOneHostedProbe(tokens, i, consumed);
		PPToken folded(PPT_PP_NUMBER, value ? "1" : "0");
		folded.ws_before = token.ws_before;
		folded.file = token.file;
		folded.line = token.line;
		out.push_back(folded);
		i += consumed;
	}
	return out;
}

// One probe invocation at tokens[at]: `name ( balanced-tokens )`.
// Returns the folded value and sets consumed to the token count of the
// whole invocation.
long Preprocessor::FoldOneHostedProbe(const vector<PPToken>& tokens,
                                      size_t at, size_t& consumed)
{
	const string& op = tokens[at].data;
	if (at + 1 >= tokens.size() || !IsOp(tokens[at + 1], "("))
		throw runtime_error("expected ( after " + op);
	size_t close = at + 2;
	int depth = 0;
	while (close < tokens.size())
	{
		if (IsOp(tokens[close], "("))
			depth++;
		else if (IsOp(tokens[close], ")"))
		{
			if (depth == 0)
				break;
			depth--;
		}
		close++;
	}
	if (close >= tokens.size())
		throw runtime_error("unterminated " + op + " operand");
	vector<PPToken> operand(tokens.begin() + at + 2,
	                        tokens.begin() + close);
	consumed = close - at + 1;
	if (op == "__has_include" || op == "__has_include_next")
		return EvaluateHasIncludeOperand(op, operand);
	return EvaluateNamedProbeOperand(op, operand);
}

// The __has_include/__has_include_next operand: a quoted string literal
// or an angle-bracketed sequence whose spellings concatenate into the
// header name. The probe reports pure existence through the same search
// the matching #include form would run.
long Preprocessor::EvaluateHasIncludeOperand(const string& op,
                                             const vector<PPToken>& operand)
{
	if (operand.empty())
		throw runtime_error("missing " + op + " operand");
	string name;
	bool angled = false;
	if (operand.size() == 1 && operand[0].kind == PPT_STRING_LITERAL)
		name = ProbeStringValue(operand[0].data);
	else if (operand.size() == 1 && operand[0].kind == PPT_HEADER_NAME)
	{
		angled = operand[0].data[0] == '<';
		name = operand[0].data.substr(1, operand[0].data.size() - 2);
	}
	else if (IsOp(operand[0], "<") &&
	         IsOp(operand[operand.size() - 1], ">"))
	{
		angled = true;
		for (size_t i = 1; i + 1 < operand.size(); i++)
			name += operand[i].data;
	}
	else
		throw runtime_error("invalid " + op + " operand");
	if (name.empty())
		throw runtime_error("empty " + op + " header name");
	int from = op == "__has_include_next" ? IncludeNextChainStart() : -1;
	return ResolveIncludeFile(name, from, angled).found ? 1 : 0;
}

// The single-name probes. Names the registry does not know (and shaped
// operands like scoped attribute names) answer 0.
long Preprocessor::EvaluateNamedProbeOperand(const string& op,
                                             const vector<PPToken>& operand)
{
	if (op == "__is_identifier")
	{
		// clang: 1 when the operand is an ordinary identifier, 0 when
		// it names a builtin/trait/keyword the compiler reserves.
		if (operand.size() != 1 || operand[0].kind != PPT_IDENTIFIER)
			return 0;
		ETokenType keyword;
		if (LookupSimpleTokenType(operand[0].data, keyword))
			return 0;
		return HostedProbeHasBuiltin(operand[0].data) ? 0 : 1;
	}
	if (operand.size() != 1 || operand[0].kind != PPT_IDENTIFIER)
		return 0;
	const string& name = operand[0].data;
	if (op == "__has_builtin")
		return HostedProbeHasBuiltin(name) ? 1 : 0;
	if (op == "__has_feature" || op == "__has_extension")
		return HostedProbeHasFeature(name) ? 1 : 0;
	if (op == "__has_attribute")
		return HostedProbeHasAttribute(name) ? 1 : 0;
	if (op == "__has_cpp_attribute")
		return HostedProbeHasCppAttribute(name) ? 1 : 0;
	return 0;  // __building_module
}
