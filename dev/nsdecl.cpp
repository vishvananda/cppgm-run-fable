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
#include "sema/decl_parser.h"
#include "sema/entity.h"
#include "sema/program.h"

// nsdecl: runs translation phases 1-7 over each command-line source
// file (the PA5 pipeline, then the PA7 semantic parse of the phase-7
// token sequence against pa7.gram) and writes the semantically
// analyzed translation unit descriptions to the outfile in the PA7
// format. Errors are undefined behaviour for PA7; any pipeline or
// parse error exits EXIT_FAILURE.
//
// The --batch-stdin worker protocol is provided by the test runner
// entry point (src/test_runner.cpp).

namespace {

// Collects the phase-7 tokens of one srcfile for the semantic parser.
struct CollectingPostTokenStream : IPostTokenStream
{
	void emit(const PostToken& token)
	{
		tokens.push_back(token);
	}

	vector<PostToken> tokens;
};

void DescribeTranslationUnit(ostream& out, const string& srcfile,
                             const vector<pair<string, string>>& predefined)
{
	CollectingPostTokenStream collector;
	PostTokenizer post_tokenizer(collector);
	Preprocessor preprocessor(post_tokenizer, predefined);
	preprocessor.ProcessSourceFile(srcfile);
	// A throwaway Program per translation unit: PA7 describes each unit
	// independently, so nothing links across units here.
	Program program;
	SemaModel model;
	DeclParser parser(collector.tokens, model, program);
	parser.ParseTranslationUnit();
	out << "start translation unit " << srcfile << "\n";
	DescribeNamespace(out, *model.global());
	out << "end translation unit\n";
}

// Recursive descent depth is proportional to declarator nesting, so
// each srcfile runs on a worker thread with a large stack (virtual
// memory; pages commit only as touched) instead of the default ~8MB.
const size_t kParseStackBytes = 512u << 20;

struct DescribeTask
{
	ostream* out;
	const string* srcfile;
	const vector<pair<string, string>>* predefined;
	bool failed;
	string message;
};

void* DescribeThreadMain(void* opaque)
{
	DescribeTask* task = static_cast<DescribeTask*>(opaque);
	try
	{
		DescribeTranslationUnit(*task->out, *task->srcfile,
		                        *task->predefined);
	}
	catch (const exception& e)
	{
		task->failed = true;
		task->message = e.what();
	}
	return 0;
}

void DescribeOnLargeStack(ostream& out, const string& srcfile,
                          const vector<pair<string, string>>& predefined)
{
	DescribeTask task;
	task.out = &out;
	task.srcfile = &srcfile;
	task.predefined = &predefined;
	task.failed = false;
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0 ||
	    pthread_attr_setstacksize(&attributes, kParseStackBytes) != 0 ||
	    pthread_create(&thread, &attributes, DescribeThreadMain, &task) != 0)
		throw runtime_error("cannot start parse worker thread");
	pthread_join(thread, 0);
	pthread_attr_destroy(&attributes);
	if (task.failed)
		throw runtime_error(task.message);
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

		out << nsrcfiles << " translation units\n";

		for (size_t i = 0; i < nsrcfiles; i++)
			DescribeOnLargeStack(out, args[i + 2], predefined);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
