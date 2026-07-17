// Command-line driver for the PA30 `abimangle` binary. Fact parsing lives in
// src/abi/abi_fact_parse.cpp and encoding in src/abi/abi_mangle_encode.cpp
// plus src/abi/abi_mangle_names.cpp.

#include "abi_mangle.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace {

struct AbimangleInvocation
{
  string outfile;
  vector<string> inputs;
};

bool has_help_arg(int argc, char ** argv)
{
  for(int i = 1; i < argc; ++i) {
    const string arg = argv[i];
    if(arg == "--help" || arg == "-h") {
      return true;
    }
  }
  return false;
}

void print_help()
{
  cout << "usage: abimangle -o <outfile> <abi-facts-file>...\n";
}

AbimangleInvocation parse_invocation(int argc, char ** argv)
{
  AbimangleInvocation invocation;
  for(int i = 1; i < argc; ++i) {
    const string arg = argv[i];
    if(arg == "-o") {
      if(i + 1 >= argc) {
        throw logic_error("missing output file after -o");
      }
      invocation.outfile = argv[++i];
      continue;
    }
    invocation.inputs.push_back(arg);
  }
  if(invocation.outfile.empty() || invocation.inputs.empty()) {
    throw logic_error("invalid usage");
  }
  return invocation;
}

int run_abimangle(int argc, char ** argv)
{
  if(has_help_arg(argc, argv)) {
    print_help();
    return EXIT_SUCCESS;
  }
  const AbimangleInvocation invocation = parse_invocation(argc, argv);
  ofstream out(invocation.outfile.c_str());
  if(!out) {
    throw logic_error("unable to open output file '" + invocation.outfile + "'");
  }
  out << abi_mangle::mangle_fact_files(invocation.inputs);
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char ** argv)
{
  try {
    return run_abimangle(argc, argv);
  } catch(const exception & e) {
    cerr << "abimangle: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
}
