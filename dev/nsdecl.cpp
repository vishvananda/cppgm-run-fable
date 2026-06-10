#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "post_token.h"
#include "post_tokenizer.h"
#include "predefined_macros.h"
#include "preprocess.h"
#include "sema/decl_parser.h"
#include "sema/entity.h"

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
	SemaModel model;
	DeclParser parser(collector.tokens, model);
	parser.ParseTranslationUnit();
	out << "start translation unit " << srcfile << "\n";
	DescribeNamespace(out, *model.global());
	out << "end translation unit\n";
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
			DescribeTranslationUnit(out, args[i + 2], predefined);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
