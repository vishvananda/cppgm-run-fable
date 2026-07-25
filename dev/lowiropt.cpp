// The PA37 `lowiropt` binary: parse LowIR text, run the deterministic
// optimization pipeline at the requested level, and write the
// canonical LowIR dump.

#include "exceptions.h"
#include "tool_help_text.h"

#include "lowir/lowir_dump.h"
#include "lowir/lowir_opt.h"
#include "lowir/lowir_parser.h"
#include "lowir/lowir_validate.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

struct LowIROptInvocation
{
  bool has_optimization_level = false;
  int optimization_level = 0;
  string outfile;
  vector<string> inputs;
};

vector<string> collect_args(int argc, char ** argv)
{
  vector<string> args;
  for(int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }
  return args;
}

bool has_help_arg(const vector<string> & args)
{
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == "--help" || args[i] == "-h") {
      return true;
    }
  }
  return false;
}

bool has_batch_stdin_arg(const vector<string> & args)
{
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == "--batch-stdin") {
      return true;
    }
  }
  return false;
}

int run_not_implemented_batch_mode()
{
  string line;
  while(getline(cin, line)) {
    (void)line;
    cout << "EXIT_NOT_IMPLEMENTED" << endl;
  }
  return EXIT_SUCCESS;
}

bool is_optimization_level(const string & arg, int & level)
{
  if(arg == "-O0") {
    level = 0;
    return true;
  }
  if(arg == "-O1") {
    level = 1;
    return true;
  }
  if(arg == "-O2") {
    level = 2;
    return true;
  }
  return false;
}

bool starts_with_dash(const string & arg)
{
  return !arg.empty() && arg[0] == '-';
}

LowIROptInvocation parse_lowiropt_invocation(const vector<string> & args)
{
  LowIROptInvocation invocation;

  for(size_t i = 0; i < args.size(); ++i) {
    int optimization_level = 0;
    if(is_optimization_level(args[i], optimization_level)) {
      if(invocation.has_optimization_level) {
        throw logic_error("multiple optimization levels provided");
      }
      invocation.has_optimization_level = true;
      invocation.optimization_level = optimization_level;
      continue;
    }
    if(args[i] == "-o") {
      if(i + 1 >= args.size()) {
        throw logic_error("missing output file after -o");
      }
      if(!invocation.outfile.empty()) {
        throw logic_error("multiple output files provided");
      }
      invocation.outfile = args[++i];
      continue;
    }
    if(starts_with_dash(args[i])) {
      throw logic_error("unknown option: " + args[i]);
    }
    invocation.inputs.push_back(args[i]);
  }

  if(!invocation.has_optimization_level ||
     invocation.outfile.empty() ||
     invocation.inputs.empty()) {
    throw logic_error("invalid usage");
  }

  return invocation;
}

string read_input_file(const string & path)
{
  ifstream in(path.c_str(), ios::binary);
  if(!in) {
    throw runtime_error("cannot read input file: " + path);
  }
  ostringstream data;
  data << in.rdbuf();
  return data.str();
}

int run_lowiropt_mode(const vector<string> & args)
{
  if(has_batch_stdin_arg(args)) {
    return run_not_implemented_batch_mode();
  }

  if(has_help_arg(args)) {
    cout << lowiropt_help_text();
    return EXIT_SUCCESS;
  }

  const LowIROptInvocation invocation = parse_lowiropt_invocation(args);

  string text;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    text += read_input_file(invocation.inputs[i]);
  }

  LowIRProgram program = ParseLowIRProgram(text);
  ValidateLowIRProgram(program, false);
  OptimizeLowIRProgram(program, invocation.optimization_level);

  ofstream out(invocation.outfile.c_str(), ios::binary);
  if(!out) {
    throw runtime_error("cannot create output file: " +
                        invocation.outfile);
  }
  DumpLowIRProgram(program, out);
  out.flush();
  if(!out) {
    throw runtime_error("cannot write output file: " +
                        invocation.outfile);
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char ** argv)
{
  try
  {
    return run_lowiropt_mode(collect_args(argc, argv));
  }
  catch(const NotImplementedException & e)
  {
    cerr << "ERROR: " << e.what() << endl;
    return CPPGM_EXIT_NOT_IMPLEMENTED;
  }
  catch(const exception & e)
  {
    cerr << "ERROR: " << e.what() << endl;
    return EXIT_FAILURE;
  }
}
