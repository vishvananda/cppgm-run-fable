// PA28 `lowir2native`: LowIR in, machine IR dump and native Linux
// executable out. The LowIR source files are concatenated in command-line
// order, parsed and validated with the PA13 front half, lowered once into
// the typed machine IR, and that single MIR program feeds both the
// deterministic dump and the ELF encoder.

#include "exceptions.h"
#include "lowir/lowir_parser.h"
#include "lowir/lowir_validate.h"
#include "mir_model.h"
#include "tool_help_text.h"
#include "x86/lowir_to_mir.h"
#include "x86/mir_to_native.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

struct LowIR2NativeInvocation
{
  bool has_optimization_level = false;
  int optimization_level = 0;
  string output_target;
  string outfile;
  string machine_ir_file;
  vector<string> srcfiles;
};

vector<string> collect_args(int argc, char ** argv)
{
  vector<string> args;
  for(int i = 1; i < argc; ++i) {
    args.push_back(argv[i]);
  }
  return args;
}

bool has_arg(const vector<string> & args, const string & needle)
{
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == needle) {
      return true;
    }
  }
  return false;
}

bool has_help_arg(const vector<string> & args)
{
  return has_arg(args, "--help") || has_arg(args, "-h");
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

bool has_batch_stdin_arg(const vector<string> & args)
{
  return has_arg(args, "--batch-stdin");
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

LowIR2NativeInvocation parse_lowir2native_invocation(const vector<string> & args)
{
  LowIR2NativeInvocation invocation;

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
    if(args[i] == "--target") {
      if(i + 1 >= args.size()) {
        throw logic_error("missing target after --target");
      }
      if(!invocation.output_target.empty()) {
        throw logic_error("multiple --target options provided");
      }
      invocation.output_target = args[++i];
      continue;
    }
    if(args[i] == "--dump-machine-ir" || args[i] == "--dump-native-plan") {
      if(i + 1 >= args.size()) {
        throw logic_error("missing output file after --dump-machine-ir");
      }
      if(!invocation.machine_ir_file.empty()) {
        throw logic_error("multiple machine IR dump paths provided");
      }
      invocation.machine_ir_file = args[++i];
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
    invocation.srcfiles.push_back(args[i]);
  }

  if((invocation.outfile.empty() && invocation.machine_ir_file.empty()) ||
     invocation.srcfiles.empty()) {
    throw logic_error("invalid usage");
  }

  return invocation;
}

string read_source_file(const string & path)
{
  ifstream in(path.c_str(), ios::in | ios::binary);
  if(!in) {
    throw runtime_error("unable to read input file: " + path);
  }
  ostringstream text;
  text << in.rdbuf();
  if(!in.good() && !in.eof()) {
    throw runtime_error("unable to read input file: " + path);
  }
  return text.str();
}

int run_lowir2native_mode(const vector<string> & args)
{
  if(has_batch_stdin_arg(args)) {
    return run_not_implemented_batch_mode();
  }

  if(has_help_arg(args)) {
    cout << lowir2native_help_text();
    return EXIT_SUCCESS;
  }

  const LowIR2NativeInvocation invocation =
      parse_lowir2native_invocation(args);

  string source;
  for(size_t i = 0; i < invocation.srcfiles.size(); ++i) {
    source += read_source_file(invocation.srcfiles[i]);
    source += "\n";
  }

  const string target =
      invocation.output_target.empty() ? "linux" : invocation.output_target;
  LowIRProgram program = ParseLowIRProgram(source);
  LowIRProgramInfo info = ValidateLowIRProgram(program, false);
  mir_model::MirProgram machine_ir =
      LowerLowIRProgramToMir(program, info, target);

  if(!invocation.machine_ir_file.empty()) {
    mir_model::write_mir_program_file(invocation.machine_ir_file,
                                      machine_ir);
  }
  if(!invocation.outfile.empty()) {
    WriteMirProgramExecutable(machine_ir, invocation.outfile);
  }
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char ** argv)
{
  try
  {
    return run_lowir2native_mode(collect_args(argc, argv));
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
