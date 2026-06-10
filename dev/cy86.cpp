#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

#include "cy86/cy86_codegen.h"
#include "cy86/cy86_parser.h"
#include "post_token.h"
#include "post_tokenizer.h"
#include "predefined_macros.h"
#include "preprocess.h"
#include "x86/elf_program.h"

// cy86: translates CY86 Mock Intermediate Language source files into
// a native Linux x86-64 ELF executable. Each srcfile runs through the
// PA5 preprocessor and the phase-7 tokenizer (so phase-6 string
// concatenation stays per translation unit); the token sequences are
// concatenated in command-line order, parsed against pa9.gram,
// translated to x86-64 machine code, and laid out into the program
// image. Diagnosable ill-formed programs exit EXIT_FAILURE.
//
// The --batch-stdin worker protocol is provided by the test runner
// entry point (src/test_runner.cpp).

namespace {

// Collects the phase-7 tokens of one srcfile; the per-file eof marker
// is dropped so the files concatenate into one program sequence.
struct CollectingPostTokenStream : IPostTokenStream
{
	void emit(const PostToken& token)
	{
		if (token.kind != PTK_EOF)
			tokens.push_back(token);
	}

	vector<PostToken> tokens;
};

}  // namespace

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

		string outfile;
		vector<string> srcfiles;
		for (size_t i = 0; i < args.size(); i++)
		{
			if (args[i] == "--target")
			{
				// The native x86-64 ELF target is the only output
				// format; the harness option is accepted for
				// compatibility.
				if (i + 1 >= args.size())
					throw logic_error("missing target after --target");
				i++;
				continue;
			}
			if (args[i] == "-o")
			{
				if (i + 1 >= args.size())
					throw logic_error("missing output file after -o");
				outfile = args[++i];
				continue;
			}
			srcfiles.push_back(args[i]);
		}
		if (outfile.empty() || srcfiles.empty())
			throw logic_error("invalid usage");

		vector<pair<string, string>> predefined = PredefinedObjectMacros();

		CollectingPostTokenStream collector;
		for (size_t i = 0; i < srcfiles.size(); i++)
		{
			PostTokenizer post_tokenizer(collector);
			Preprocessor preprocessor(post_tokenizer, predefined);
			preprocessor.ProcessSourceFile(srcfiles[i]);
		}

		CY86Program program;
		ParseCY86Program(collector.tokens, program);

		ProgramImage image;
		TranslateCY86Program(program, image);
		image.WriteExecutable(outfile);

		return EXIT_SUCCESS;
	}
	catch (exception& e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
