#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

#include "ctrl_expr.h"
#include "pp_tokenizer.h"
#include "source_translation.h"
#include "tool_stdin.h"

// ctrlexpr: applies translation phases 1-3 to the C++ source file on
// stdin (no preprocessing directives, predefined macros, or pragma
// operator), splits the preprocessing-token stream into logical lines,
// and evaluates each non-empty line as a conditional-inclusion
// controlling expression in the PA3 output format. Phase 1-3 failures
// exit EXIT_FAILURE; all other problems print per-line "error".
//
// The --batch-stdin worker protocol is provided by the test runner entry
// point (src/test_runner.cpp); this tool only evaluates a single file.

// mock implementation of IsDefinedIdentifier for PA3
// return true iff first code point is odd
bool PA3Mock_IsDefinedIdentifier(const string& identifier)
{
	if (identifier.empty())
		return false;
	else
		return identifier[0] % 2;
}

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

		CtrlExprStream evaluator(cout, &PA3Mock_IsDefinedIdentifier);
		TokenizePPTokens(source, evaluator);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
