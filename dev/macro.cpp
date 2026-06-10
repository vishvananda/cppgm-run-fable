#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;

#include "macro_preprocess.h"
#include "post_token.h"
#include "post_tokenizer.h"
#include "pp_token.h"
#include "pp_tokenizer.h"
#include "source_translation.h"

// macro: applies translation phases 1-6 and the tokenization part of
// phase 7 to the C++ source file on stdin. Phase 4 handles the PA4 input
// class (only #define/#undef directives, no predefined macros, no pragma
// operator); text-sequences are macro-replaced per 16.3 with the
// course-defined nesting semantics and the result is written in the PA2
// posttoken format. Errors exit EXIT_FAILURE.
//
// The --batch-stdin worker protocol is provided by the test runner entry
// point (src/test_runner.cpp); this tool only processes a single file.

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

		PPTokenCollector collector;
		TokenizePPTokens(source, collector);

		PrintingPostTokenStream printer;
		PostTokenizer post_tokenizer(printer);
		MacroPreprocessor preprocessor(post_tokenizer);
		preprocessor.ProcessFile(collector.tokens);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
