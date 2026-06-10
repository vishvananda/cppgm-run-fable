#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pthread.h>

using namespace std;

#include "post_token.h"
#include "post_tokenizer.h"
#include "predefined_macros.h"
#include "preprocess.h"
#include "parse/parse_token.h"
#include "parse/parser.h"

// recog: runs the PA5 pipeline (translation phases 1-6 plus the
// tokenization part of phase 7) over each command-line source file and
// recognizes the token sequence against the pa6.gram translation-unit
// grammar. The outfile gets `recog <N>` followed by one
// `<srcfile> OK|BAD` line per file; any per-file error (open failure,
// PA5 error, phase-7 invalid token, parse failure) makes that file BAD
// without failing the tool. The parse tree goes to stdout on success
// (debugging output; only the outfile and exit status are gated).
//
// The --batch-stdin worker protocol is provided by the test runner
// entry point (src/test_runner.cpp).

namespace {

// Collects the phase-7 tokens of one srcfile for the parser; invalid
// tokens are kept and rejected by BuildParseTokens.
struct CollectingPostTokenStream : IPostTokenStream
{
	void emit(const PostToken& token)
	{
		tokens.push_back(token);
	}

	vector<PostToken> tokens;
};

// True when the srcfile preprocesses, tokenizes, and matches
// translation-unit; throws (BAD) on pipeline errors.
bool RecognizeSourceFile(const string& srcfile,
                         const vector<pair<string, string>>& predefined)
{
	CollectingPostTokenStream collector;
	PostTokenizer post_tokenizer(collector);
	Preprocessor preprocessor(post_tokenizer, predefined);
	preprocessor.ProcessSourceFile(srcfile);
	vector<ParseToken> tokens = BuildParseTokens(collector.tokens);
	Parser parser(tokens);
	ParseNodePtr tree = parser.ParseTranslationUnit();
	if (!tree)
		return false;
	PrintParseTree(*tree, cout);
	return true;
}

// Recursive descent depth is proportional to input nesting, so each
// srcfile runs on a worker thread with a large stack (virtual memory;
// pages commit only as touched) instead of the default ~8MB.
const size_t kParseStackBytes = 512u << 20;

struct RecognizeTask
{
	const string* srcfile;
	const vector<pair<string, string>>* predefined;
	bool ok;
	bool failed;
	string message;
};

void* RecognizeThreadMain(void* opaque)
{
	RecognizeTask* task = static_cast<RecognizeTask*>(opaque);
	try
	{
		task->ok = RecognizeSourceFile(*task->srcfile, *task->predefined);
	}
	catch (const exception& e)
	{
		task->failed = true;
		task->message = e.what();
	}
	return 0;
}

bool RecognizeOnLargeStack(const string& srcfile,
                           const vector<pair<string, string>>& predefined)
{
	RecognizeTask task;
	task.srcfile = &srcfile;
	task.predefined = &predefined;
	task.ok = false;
	task.failed = false;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0 ||
	    pthread_attr_setstacksize(&attributes, kParseStackBytes) != 0 ||
	    pthread_create(&thread, &attributes, RecognizeThreadMain, &task) != 0)
		throw runtime_error("cannot start parse worker thread");
	pthread_join(thread, 0);
	pthread_attr_destroy(&attributes);
	if (task.failed)
		throw runtime_error(task.message);
	return task.ok;
}

} // namespace

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		if (string(argv[i]) == "--batch-stdin")
		{
			cerr << "ERROR: --batch-stdin requires the test runner build "
			        "(CPPGM_TEST_RUNNER=1)" << endl;
			return EXIT_FAILURE;
		}
	}

	try
	{
		vector<string> args;
		for (int i = 1; i < argc; i++)
			args.emplace_back(argv[i]);

		if (args.size() < 3 || args[0] != "-o")
			throw logic_error("invalid usage");

		string outfile = args[1];
		size_t nsrcfiles = args.size() - 2;

		vector<pair<string, string>> predefined = PredefinedObjectMacros();

		ofstream out(outfile.c_str());
		if (!out)
			throw runtime_error("cannot create output file: " + outfile);

		out << "recog " << nsrcfiles << endl;

		for (size_t i = 0; i < nsrcfiles; i++)
		{
			string srcfile = args[i + 2];
			bool ok = false;
			try
			{
				ok = RecognizeOnLargeStack(srcfile, predefined);
			}
			catch (const exception& e)
			{
				cerr << e.what() << endl;
			}
			out << srcfile << (ok ? " OK" : " BAD") << endl;
		}

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
