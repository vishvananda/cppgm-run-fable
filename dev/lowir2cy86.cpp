#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

#include "lowir/lowir_parser.h"
#include "lowir/lowir_to_cy86.h"
#include "lowir/lowir_validate.h"
#include "tool_help_text.h"

// lowir2cy86: the PA13 backend adapter. The LowIR source files are
// concatenated in command-line order into one program, parsed against
// pa13.gram, structurally validated, and translated into deterministic
// PA9 CY86 source text written to the output file. Diagnosable
// ill-formed programs exit EXIT_FAILURE.
//
// The --batch-stdin worker protocol is provided by the test runner
// entry point (src/test_runner.cpp).

namespace {

string read_source_file(const string & path)
{
	ifstream in(path.c_str(), ios::in | ios::binary);
	if(!in)
		throw runtime_error("unable to read input file: " + path);
	ostringstream text;
	text << in.rdbuf();
	if(!in.good() && !in.eof())
		throw runtime_error("unable to read input file: " + path);
	return text.str();
}

}  // namespace

int main(int argc, char ** argv)
{
	for(int i = 1; i < argc; i++)
	{
		if(string(argv[i]) == "--batch-stdin")
		{
			cerr << "ERROR: --batch-stdin requires the test runner build "
			        "(CPPGM_TEST_RUNNER=1)" << endl;
			return EXIT_FAILURE;
		}
		if(string(argv[i]) == "--help" || string(argv[i]) == "-h")
		{
			cout << lowir2cy86_help_text();
			return EXIT_SUCCESS;
		}
	}

	try
	{
		vector<string> args;
		for(int i = 1; i < argc; i++)
			args.emplace_back(argv[i]);

		if(args.size() < 3 || args[0] != "-o")
			throw logic_error("invalid usage");
		const string outfile = args[1];

		// Multiple translation units form one LowIR program in
		// command-line order.
		string source;
		for(size_t i = 2; i < args.size(); i++)
		{
			source += read_source_file(args[i]);
			source += "\n";
		}

		LowIRProgram program = ParseLowIRProgram(source);
		LowIRProgramInfo info = ValidateLowIRProgram(program);
		string cy86_text = TranslateLowIRProgramToCY86(program, info);

		ofstream out(outfile.c_str(), ios::out | ios::binary);
		if(!out)
			throw runtime_error("unable to write output file: " + outfile);
		out << cy86_text;
		out.close();
		if(!out.good())
			throw runtime_error("unable to write output file: " + outfile);

		return EXIT_SUCCESS;
	}
	catch(const exception & e)
	{
		cerr << "ERROR: " << e.what() << endl;
		return EXIT_FAILURE;
	}
}
