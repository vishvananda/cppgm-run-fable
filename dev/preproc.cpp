#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "post_token.h"
#include "post_tokenizer.h"
#include "preprocess.h"

// preproc: executes translation phases 1-6 and the tokenization part of
// phase 7 over each command-line source file (a complete preprocessor
// and lexer for the PA5 input class) and describes the token sequences
// to the output file: `preproc <N>`, then per srcfile `sof <name>`, the
// PA2 posttoken lines, and `eof`. Each srcfile is preprocessed with
// fresh state; only the asctime-derived __DATE__/__TIME__ values are
// captured once at entry. Any error exits EXIT_FAILURE (the outfile
// state is undefined on failure).
//
// The --batch-stdin worker protocol is provided by the test runner entry
// point (src/test_runner.cpp).

namespace {

// Writes posttoken lines to the outfile. A phase-7 invalid token is a
// PA5 error rather than output.
struct FilePostTokenStream : IPostTokenStream
{
	explicit FilePostTokenStream(ostream& out) : out_(out) {}

	void emit(const PostToken& token)
	{
		if (token.kind == PTK_INVALID)
			throw runtime_error("invalid token at phase 7: " + token.source);
		out_ << DescribePostToken(token) << "\n";
	}

	ostream& out_;
};

// The course-defined predefined macros with fixed replacement lists;
// __FILE__/__LINE__ are installed by the Preprocessor as builtins.
// __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss") slice the asctime
// format "Www Mmm dd hh:mm:ss yyyy\n", called once here at main entry.
vector<pair<string, string>> PredefinedObjectMacros()
{
	time_t now = time(0);
	string stamp = asctime(localtime(&now));
	vector<pair<string, string>> macros;
	macros.push_back(make_pair("__CPPGM__", "201303L"));
	macros.push_back(make_pair("__cplusplus", "201103L"));
	macros.push_back(make_pair("__STDC_HOSTED__", "1"));
	macros.push_back(make_pair("__CPPGM_AUTHOR__",
	                           "\"Vishvananda Abrams\""));
	macros.push_back(make_pair("__DATE__", "\"" + stamp.substr(4, 7) +
	                                       stamp.substr(20, 4) + "\""));
	macros.push_back(make_pair("__TIME__",
	                           "\"" + stamp.substr(11, 8) + "\""));
	return macros;
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

		out << "preproc " << nsrcfiles << "\n";

		for (size_t i = 0; i < nsrcfiles; i++)
		{
			string srcfile = args[i + 2];
			out << "sof " << srcfile << "\n";
			FilePostTokenStream printer(out);
			PostTokenizer post_tokenizer(printer);
			Preprocessor preprocessor(post_tokenizer, predefined);
			preprocessor.ProcessSourceFile(srcfile);
		}

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
