#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "post_token.h"
#include "post_tokenizer.h"
#include "pp_tokenizer.h"
#include "source_translation.h"

// posttoken: applies translation phases 1-6 and the tokenization part of
// phase 7 to the C++ source file on stdin (which contains no
// preprocessing directives, so phase 4 is a no-op) and writes the
// resulting token sequence to stdout in the PA2 format.
//
// The --batch-stdin worker protocol is provided by the test runner entry
// point (src/test_runner.cpp); this tool only tokenizes a single file.

namespace {

struct PrintingPostTokenStream : IPostTokenStream
{
	void emit(const PostToken& token)
	{
		cout << DescribePostToken(token) << endl;
	}
};

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
		ostringstream oss;
		oss << cin.rdbuf();

		TranslatedSource source = TranslateSource(oss.str());

		PrintingPostTokenStream printer;
		PostTokenizer post_tokenizer(printer);
		TokenizePPTokens(source, post_tokenizer);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
