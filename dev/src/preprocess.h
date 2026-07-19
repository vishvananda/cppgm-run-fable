#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

using std::pair;
using std::set;
using std::string;
using std::vector;

#include "IPPTokenStream.h"
#include "macro_expand.h"
#include "macro_table.h"
#include "pp_token.h"

// System-wide unique file identity (device id, inode number) used for
// #pragma once and include-skip checks, per the PA5 starter kit.
typedef pair<unsigned long, unsigned long> PreprocFileId;

// True iff a file exists at `path` (stat system call); sets out_fileid.
bool GetPreprocFileId(const string& path, PreprocFileId& out_fileid);

// Translation phase 4 for the PA5 input class: the full preprocessing
// directive set over one source file (conditional inclusion via the PA3
// evaluator, #include with the course-defined two-path search, #define/
// #undef via the PA4 macro table, #line, #error, #pragma once, null
// directives and non-directives, and the _Pragma operator), with
// physical file/line tracking for __FILE__/__LINE__. Replaced
// text-sequences stream into `output` (the PA2 PostTokenizer) as one
// per-srcfile token stream, so phase-6 string concatenation spans
// directive, include, and inactive-section boundaries; one eof is
// emitted at the end of the source file. Every PA5 error throws
// std::runtime_error (the tool exits EXIT_FAILURE).
//
// One Preprocessor instance processes one srcfile: the handout requires
// no shared state between srcfiles, so the tool constructs a fresh
// instance (macro table, conditional state, pragma-once set, file
// instances) per file.
class Preprocessor : public IBuiltinTokenSource
{
public:
	// `object_macros` holds (name, replacement spelling) for the fixed
	// predefined macros (__CPPGM__, __DATE__, ...); the spellings are
	// tokenized through the real phase 1-3 pipeline. __FILE__ and
	// __LINE__ are registered as builtins here.
	Preprocessor(IPPTokenStream& output,
	             const vector<pair<string, string>>& object_macros);

	// PA29: user include search directories (-I), tried after the
	// including file's directory and before the working directory.
	void AddIncludeDir(const string& dir);

	// PA34 hosted mode: enables the hosted preprocessor surface --
	// the __has_* probe operators in controlling expressions (their
	// names also read as defined), #include_next, #warning, and the
	// system include chain below.
	void EnableHostedMode();

	// PA34: system include directories (-isystem, then the hosted
	// environment's standard paths), searched after the working
	// directory. Together with the -I directories they form the
	// ordered chain #include_next resumes in.
	void AddSystemIncludeDir(const string& dir);

	// PA34: a driver -include file, processed inside the main source
	// file's token stream before its first token.
	void AddPreIncludeFile(const string& path);

	// PA34: one host-probe `#define` body ("name replacement" /
	// "name(params) replacement"). Names already defined are skipped so
	// the course predefined set and earlier imports stay authoritative.
	void DefineHostMacroLine(const string& line);

	// PA34: driver -D spec ("name", "name=", "name=value"); overrides
	// any existing definition silently, like the host driver.
	void DefineCommandLineMacro(const string& spec);

	// PA34: driver -U name; undefining an unknown name is a no-op.
	void UndefCommandLineMacro(const string& name);

	// Runs phases 1-4 over the source file named on the command line.
	void ProcessSourceFile(const string& path);

	// PA29: runs phases 1-4 over in-memory source text (the built-in
	// runtime library translation unit). `presumed_name` is the
	// __FILE__ spelling; it contributes no directory to include search.
	void ProcessSourceText(const string& presumed_name,
	                       const string& bytes);

	// IBuiltinTokenSource: __FILE__/__LINE__ values for an invocation
	// through `head`, from its position stamp and its file instance's
	// current presumed state.
	PPToken MakeBuiltinToken(EMacroBuiltin builtin, const PPToken& head);

private:
	// One entry into a file: the srcfile or one #include occurrence.
	// Append-only so position stamps on stored tokens (e.g. replacement
	// lists defined inside an include) stay resolvable after the include
	// exits. presumed_name and line_offset are the mutable #line state:
	// presumed line = physical line + line_offset. search_chain_index is
	// the -I/system chain slot that produced the file (-1 for the main
	// file and file-relative or working-directory finds); #include_next
	// resumes strictly after it.
	struct FileInstance
	{
		string presumed_name;
		long line_offset;
		int search_chain_index;
	};

	// Outcome of one include search (PA34: shared by #include,
	// #include_next, and the __has_include probes).
	struct IncludeResolution
	{
		IncludeResolution() : found(false), chain_index(-1) {}

		bool found;
		string path;
		PreprocFileId fileid;
		int chain_index;
	};

	// One conditional-inclusion group (16.1). `parent_active` is the
	// activity of the enclosing section when the #if arrived: ordering
	// violations are errors in any context, but expressions are only
	// evaluated (and #else/#endif extra-token checks applied) under an
	// active parent.
	struct CondGroup
	{
		bool parent_active;
		bool taken;
		bool seen_else;
		bool active;
	};

	bool IsActive() const;
	vector<PPToken> TokenizeSource(const string& bytes, int file_index);
	void ProcessFileTokens(vector<PPToken> tokens, int file_index);
	void ProcessDirective(const vector<PPToken>& line,
	                      const PPToken& terminator);
	void FlushText(vector<PPToken>& text);

	void HandleIf(const vector<PPToken>& args);
	void HandleIfdef(const vector<PPToken>& args, bool negate);
	void HandleElif(const vector<PPToken>& args);
	void HandleElse(const vector<PPToken>& args);
	void HandleEndif(const vector<PPToken>& args);
	bool EvaluateCondition(const vector<PPToken>& tokens);
	vector<PPToken> FoldDefinedOperators(const vector<PPToken>& tokens);
	bool LookupDefinedName(const vector<PPToken>& args) const;

	void HandleInclude(const vector<PPToken>& args, bool include_next);
	void HandleLine(const vector<PPToken>& args, const PPToken& terminator);
	void HandlePragma(const vector<PPToken>& args);
	void ExecutePragmaOperator(const string& spelling);
	void PragmaOnceCurrentFile();

	void DefineObjectMacro(const string& name, const string& spelling);

	// The include search (PA5/PA29 order, PA34 system chain appended):
	// including file's directory (quoted position, skipped for
	// include_next), -I chain, working directory, system chain.
	// from_chain_index >= 0 restricts the search to chain entries at or
	// after that slot (the #include_next resume point).
	IncludeResolution ResolveIncludeFile(const string& name,
	                                     int from_chain_index,
	                                     bool angled = false) const;
	void ProcessIncludedFile(const IncludeResolution& resolved);
	int IncludeNextChainStart() const;

	// PA34 hosted handlers (preprocess_hosted.cpp).
	static bool IsHostedProbeName(const string& name);
	vector<PPToken> FoldHostedProbeOperators(const vector<PPToken>& tokens);
	long FoldOneHostedProbe(const vector<PPToken>& tokens, size_t at,
	                        size_t& consumed);
	long EvaluateHasIncludeOperand(const string& op,
	                               const vector<PPToken>& operand);
	long EvaluateNamedProbeOperand(const string& op,
	                               const vector<PPToken>& operand);
	void HandleWarning(const vector<PPToken>& args,
	                   const PPToken& terminator);
	void ProcessPreIncludeFiles();

	MacroTable table_;
	MacroExpander expander_;
	IPPTokenStream& output_;
	vector<string> include_dirs_;
	vector<string> system_include_dirs_;
	vector<string> pre_include_files_;
	vector<FileInstance> files_;
	vector<CondGroup> conds_;
	set<PreprocFileId> pragma_onced_;
	int current_file_;
	size_t cond_base_;
	int include_depth_;
	bool hosted_;
};
