#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

#include "IPPTokenStream.h"
#include "DebugPPTokenStream.h"
#include "pp_tokenizer.h"
#include "source_translation.h"
#include "tool_stdin.h"

// pptoken: applies translation phases 1-3 to the C++ source file on stdin
// and writes the resulting preprocessing-token sequence to stdout.
//
// The --batch-stdin worker protocol is provided by the test runner entry
// point (src/test_runner.cpp); this tool only lexes a single file.

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
		TranslatedSource source = TranslateSource(ReadAllStdin());

		DebugPPTokenStream output;
		TokenizePPTokens(source, output);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
