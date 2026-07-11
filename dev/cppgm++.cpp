// The `cppgm++` source compiler. PA10 implements --emit-ast: run
// translation phases 1-7 over each srcfile, parse each translation
// unit with the pa10.gram tree-building parser, and write the
// deterministic AST dump. PA11 implements --emit-types: bind each
// parsed unit's declarations into the scope/type model and write the
// deterministic scope-tree dump.

#include "exceptions.h"
#include "tool_help_text.h"

#include "ast/ast.h"
#include "ast/ast_parser.h"
#include "ast/ast_printer.h"
#include "lowering/lower_program.h"
#include "parse/parse_token.h"
#include "post_token.h"
#include "post_tokenizer.h"
#include "predefined_macros.h"
#include "preprocess.h"
#include "sema/decl_binder.h"
#include "sema/scope.h"
#include "sema/sem_binder.h"
#include "sema/sem_node.h"
#include "toolchain/compile_unit.h"
#include "toolchain/elf_reader.h"
#include "toolchain/link_executable.h"
#include "toolchain/object_module.h"
#include "toolchain/runtime_library.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pthread.h>

using namespace std;

namespace {

enum class EmitMode
{
  None,
  Ast,
  Types,
  Semantics,
  LowIR,
};

enum class DriverMode
{
  Query,
  Preprocess,
  Compile,
  Link,
};

struct DriverInvocation
{
  DriverMode mode;
  string outfile;
  bool explicit_outfile;
  string target;
  vector<string> include_dirs;
  vector<string> lib_dirs;
  vector<string> libs;
  vector<string> inputs;

  DriverInvocation()
      : mode(DriverMode::Link), explicit_outfile(false)
  {
  }
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

bool starts_with(const string & value, const string & prefix)
{
  return value.size() >= prefix.size() &&
      value.compare(0, prefix.size(), prefix) == 0;
}

bool is_query_driver_flag(const string & arg)
{
  return arg == "--version" ||
      arg == "-v" ||
      arg == "-dumpmachine" ||
      arg == "-dumpversion" ||
      arg == "-print-search-dirs";
}

bool is_optimization_flag(const string & arg)
{
  return starts_with(arg, "-O");
}

bool is_debug_info_flag(const string & arg)
{
  return arg == "-g0" ||
      arg == "-gline-tables-only" ||
      arg == "-g" ||
      starts_with(arg, "-g");
}

bool is_benign_driver_flag(const string & arg)
{
  return arg == "-Wall" ||
      arg == "-Winvalid-offsetof" ||
      arg == "-pipe" ||
      arg == "-w" ||
      arg == "-pg" ||
      arg == "-pedantic" ||
      arg == "-pedantic-errors" ||
      starts_with(arg, "-W") ||
      starts_with(arg, "-f") ||
      starts_with(arg, "-m") ||
      starts_with(arg, "-std=");
}

logic_error missing_option_argument(const string & option,
                                    const string & expected)
{
  return logic_error("missing " + expected + " after " + option);
}

void consume_required_option_argument(const vector<string> & args,
                                      size_t & i,
                                      const string & option,
                                      const string & expected)
{
  if(i + 1 >= args.size()) {
    throw missing_option_argument(option, expected);
  }
  ++i;
}

bool consume_joined_or_separate_option(const vector<string> & args,
                                       size_t & i,
                                       const string & option,
                                       const string & expected)
{
  if(args[i] == option) {
    consume_required_option_argument(args, i, option, expected);
    return true;
  }
  if(starts_with(args[i], option) && args[i].size() > option.size()) {
    return true;
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

void consume_emit_flag(vector<string> & args,
                       const string & flag,
                       EmitMode value,
                       EmitMode & out)
{
  vector<string> kept;
  bool found = false;
  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == flag) {
      found = true;
      continue;
    }
    kept.push_back(args[i]);
  }

  if(!found) {
    return;
  }

  if(out != EmitMode::None) {
    throw logic_error("multiple --emit-* options provided");
  }
  out = value;
  args.swap(kept);
}

EmitMode parse_emit_mode(vector<string> & args)
{
  EmitMode mode = EmitMode::None;
  consume_emit_flag(args, "--emit-ast", EmitMode::Ast, mode);
  consume_emit_flag(args, "--emit-types", EmitMode::Types, mode);
  consume_emit_flag(args, "--emit-semantics", EmitMode::Semantics, mode);
  consume_emit_flag(args, "--emit-lowir", EmitMode::LowIR, mode);
  return mode;
}

struct SourceOutputInvocation
{
  string outfile;
  vector<string> inputs;
};

SourceOutputInvocation parse_source_output_invocation(
    const vector<string> & args,
    bool allow_lowir_options)
{
  SourceOutputInvocation invocation;
  bool explicit_outfile = false;

  for(size_t i = 0; i < args.size(); ++i) {
    if(args[i] == "-o") {
      consume_required_option_argument(args, i, "-o", "output file");
      invocation.outfile = args[i];
      explicit_outfile = true;
      continue;
    }
    if(allow_lowir_options &&
       (is_optimization_flag(args[i]) || is_debug_info_flag(args[i]))) {
      continue;
    }
    if(allow_lowir_options &&
       (args[i] == "--witness" || args[i] == "--witness-debug")) {
      consume_required_option_argument(args, i, args[i], "output file");
      continue;
    }
    if(args[i] == "-c" || args[i] == "-E" || is_query_driver_flag(args[i])) {
      throw logic_error("invalid usage");
    }
    if(starts_with(args[i], "-")) {
      throw logic_error("unsupported option in emit mode: " + args[i]);
    }
    invocation.inputs.push_back(args[i]);
  }

  if(!explicit_outfile || invocation.inputs.empty()) {
    throw logic_error("invalid usage");
  }
  return invocation;
}

bool consume_preprocess_option(const vector<string> & args, size_t & i)
{
  if(consume_joined_or_separate_option(args, i, "-D", "macro definition")) {
    return true;
  }
  if(consume_joined_or_separate_option(args, i, "-U", "macro name")) {
    return true;
  }
  if(args[i] == "-include") {
    consume_required_option_argument(args, i, "-include", "file");
    return true;
  }
  return false;
}

// Joined-or-separate option value collection: `-I dir` and `-Idir`.
bool collect_joined_or_separate_option(const vector<string> & args,
                                       size_t & i,
                                       const string & option,
                                       const string & expected,
                                       vector<string> & values)
{
  if(args[i] == option) {
    consume_required_option_argument(args, i, option, expected);
    values.push_back(args[i]);
    return true;
  }
  if(starts_with(args[i], option) && args[i].size() > option.size()) {
    values.push_back(args[i].substr(option.size()));
    return true;
  }
  return false;
}

bool consume_search_option(const vector<string> & args, size_t & i,
                           DriverInvocation & invocation)
{
  if(collect_joined_or_separate_option(args, i, "-I", "path",
                                       invocation.include_dirs)) {
    return true;
  }
  if(consume_joined_or_separate_option(args, i, "-isystem", "path")) {
    return true;
  }
  if(collect_joined_or_separate_option(args, i, "-L", "path",
                                       invocation.lib_dirs)) {
    return true;
  }
  if(collect_joined_or_separate_option(args, i, "-l", "library name",
                                       invocation.libs)) {
    return true;
  }
  return false;
}

bool consume_dependency_option(const vector<string> & args, size_t & i)
{
  if(args[i] == "-MMD" || args[i] == "-MD" || args[i] == "-MP") {
    return true;
  }
  if(consume_joined_or_separate_option(args, i, "-MF", "depfile path")) {
    return true;
  }
  if(consume_joined_or_separate_option(args, i, "-MT", "target")) {
    return true;
  }
  if(consume_joined_or_separate_option(args, i, "-MQ", "target")) {
    return true;
  }
  return false;
}

bool consume_toolchain_option(const vector<string> & args, size_t & i,
                              DriverInvocation & invocation)
{
  if(is_debug_info_flag(args[i])) {
    return true;
  }
  if(is_optimization_flag(args[i])) {
    return true;
  }
  if(args[i] == "--target") {
    consume_required_option_argument(args, i, "--target", "target");
    invocation.target = args[i];
    return true;
  }
  if(starts_with(args[i], "--target=")) {
    if(args[i].size() == string("--target=").size()) {
      throw missing_option_argument("--target", "target");
    }
    invocation.target = args[i].substr(string("--target=").size());
    return true;
  }
  if(args[i] == "-std") {
    consume_required_option_argument(args, i, "-std", "language standard");
    return true;
  }
  if(args[i] == "-stdlib") {
    consume_required_option_argument(args, i, "-stdlib", "standard library name");
    return true;
  }
  if(starts_with(args[i], "-stdlib=")) {
    return true;
  }
  if(args[i] == "-pthread") {
    throw logic_error("option not yet supported: -pthread");
  }
  return false;
}

DriverInvocation parse_driver_invocation(const vector<string> & args)
{
  if(args.empty()) {
    throw logic_error("invalid usage");
  }

  DriverInvocation invocation;
  if(is_query_driver_flag(args[0])) {
    if(args.size() != 1) {
      throw logic_error("query flag must be used as a direct invocation");
    }
    invocation.mode = DriverMode::Query;
    return invocation;
  }

  bool compile_only = false;
  bool preprocess_only = false;

  for(size_t i = 0; i < args.size(); ++i) {
    if(is_query_driver_flag(args[i])) {
      throw logic_error("query flag must be used as a direct invocation");
    }
    if(args[i] == "-c") {
      compile_only = true;
      continue;
    }
    if(args[i] == "-E") {
      preprocess_only = true;
      continue;
    }
    if(args[i] == "-o") {
      consume_required_option_argument(args, i, "-o", "output file");
      invocation.outfile = args[i];
      invocation.explicit_outfile = true;
      continue;
    }
    if(consume_preprocess_option(args, i) ||
       consume_search_option(args, i, invocation) ||
       consume_dependency_option(args, i) ||
       consume_toolchain_option(args, i, invocation) ||
       is_benign_driver_flag(args[i])) {
      continue;
    }
    if(starts_with(args[i], "-")) {
      throw logic_error("unsupported driver option: " + args[i]);
    }
    invocation.inputs.push_back(args[i]);
  }

  if(compile_only && preprocess_only) {
    throw logic_error("cannot combine -c and -E");
  }
  if(invocation.inputs.empty()) {
    throw logic_error("invalid usage");
  }
  if((compile_only || preprocess_only) && invocation.explicit_outfile &&
     invocation.inputs.size() != 1) {
    throw logic_error("cannot specify -o when generating multiple output files");
  }

  invocation.mode =
      preprocess_only ? DriverMode::Preprocess :
      compile_only ? DriverMode::Compile :
      DriverMode::Link;
  return invocation;
}

// -c output default: the source stem with the .obj suffix, beside the
// working directory (matching the one-file contract).
string default_object_outfile(const string & srcfile)
{
  size_t slash = srcfile.rfind('/');
  string base = slash == string::npos ? srcfile : srcfile.substr(slash + 1);
  size_t dot = base.rfind('.');
  if(dot != string::npos && dot != 0) {
    base = base.substr(0, dot);
  }
  return base + ".obj";
}

toolchain::CompileOptions make_compile_options(
    const DriverInvocation & invocation)
{
  toolchain::CompileOptions options;
  options.include_dirs = invocation.include_dirs;
  options.target = toolchain::NormalizeTargetName(invocation.target);
  return options;
}

int run_compile_mode(const DriverInvocation & invocation)
{
  if(invocation.inputs.size() != 1) {
    throw logic_error("compile mode takes exactly one source file");
  }
  const toolchain::CompileOptions options = make_compile_options(invocation);
  toolchain::ObjectModule module =
      toolchain::CompileSourceFileToModule(invocation.inputs[0], options);
  const string outfile = invocation.explicit_outfile
      ? invocation.outfile
      : default_object_outfile(invocation.inputs[0]);
  toolchain::WriteObjectModuleFile(outfile, module);
  return EXIT_SUCCESS;
}

// Object-like link inputs are classified by content: cppgm compiler
// objects and host ELF relocatables are both accepted.
toolchain::ObjectModule load_object_input(const string & path,
                                          const string & target)
{
  const string bytes = toolchain::ReadFileBytes(path);
  if(toolchain::IsCppgmObjectBytes(bytes)) {
    return toolchain::ParseObjectModuleBytes(bytes, path);
  }
  if(toolchain::IsElfObjectBytes(bytes)) {
    return toolchain::ParseElfObjectBytes(bytes, path, target);
  }
  throw logic_error("unrecognized object file format: " + path);
}

string find_library_object(const DriverInvocation & invocation,
                           const string & name)
{
  static const char * const suffixes[] = {".o", ".obj"};
  for(size_t d = 0; d < invocation.lib_dirs.size(); ++d) {
    string dir = invocation.lib_dirs[d];
    if(!dir.empty() && dir[dir.size() - 1] != '/') {
      dir += '/';
    }
    for(size_t s = 0; s < 2; ++s) {
      const string candidate = dir + "lib" + name + suffixes[s];
      ifstream probe(candidate.c_str(), ios::in | ios::binary);
      if(probe) {
        return candidate;
      }
    }
  }
  throw logic_error("cannot find library: -l" + name);
}

int run_link_mode(const DriverInvocation & invocation)
{
  const toolchain::CompileOptions options = make_compile_options(invocation);
  vector<toolchain::LinkInput> linked;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    toolchain::LinkInput input;
    input.name = invocation.inputs[i];
    if(toolchain::HasObjectFileName(invocation.inputs[i])) {
      input.module = load_object_input(invocation.inputs[i],
                                       options.target);
    }
    else {
      input.module = toolchain::CompileSourceFileToModule(
          invocation.inputs[i], options);
    }
    linked.push_back(input);
  }
  for(size_t i = 0; i < invocation.libs.size(); ++i) {
    toolchain::LinkInput input;
    input.name = find_library_object(invocation, invocation.libs[i]);
    input.module = load_object_input(input.name, options.target);
    linked.push_back(input);
  }

  // Runtime names (EH entry points, RTTI anchors, operator new) come
  // from the built-in runtime library, compiled through the same
  // pipeline when the link still needs definitions.
  if(!toolchain::UnresolvedExternals(linked).empty()) {
    toolchain::LinkInput runtime;
    runtime.name = toolchain::RuntimeLibraryName();
    runtime.module = toolchain::CompileSourceTextToModule(
        toolchain::RuntimeLibraryName(), toolchain::RuntimeLibrarySource(),
        options);
    linked.push_back(runtime);
  }

  const string outfile =
      invocation.explicit_outfile ? invocation.outfile : string("a.out");
  toolchain::LinkExecutable(linked, outfile, options.target);
  return EXIT_SUCCESS;
}

int run_unimplemented_mode(const char * feature,
                           const char * owner)
{
  (void)feature;
  (void)owner;
  throw NotImplementedException();
}

// Collects the phase-7 tokens of one srcfile for the parser; invalid
// tokens are kept and rejected by BuildParseTokens.
struct CollectingPostTokenStream : IPostTokenStream
{
  void emit(const PostToken & token)
  {
    tokens.push_back(token);
  }

  vector<PostToken> tokens;
};

// Runs translation phases 1-7 over one srcfile and parses the token
// sequence as a pa10.gram translation-unit. Throws on any pipeline or
// parse error.
AstDeclPtr parse_source_file_ast(
    const string & srcfile,
    const vector<pair<string, string>> & predefined)
{
  CollectingPostTokenStream collector;
  PostTokenizer post_tokenizer(collector);
  Preprocessor preprocessor(post_tokenizer, predefined);
  preprocessor.ProcessSourceFile(srcfile);
  vector<ParseToken> tokens = BuildParseTokens(collector.tokens);
  AstParser parser(tokens);
  return parser.ParseTranslationUnit();
}

// Recursive descent depth is proportional to input nesting, so each
// srcfile parses (and binds) on a worker thread with a large stack
// (virtual memory; pages commit only as touched).
const size_t cppgm_parse_stack_bytes = 512u << 20;

enum class BindMode
{
  ParseOnly,
  Types,
  Semantics,
};

struct EmitAstTask
{
  const string * srcfile;
  const vector<pair<string, string>> * predefined;
  BindMode bind_mode;
  AstDeclPtr unit;
  unique_ptr<TypesModel> model;
  unique_ptr<SemUnit> semantics;
  bool failed;
  string message;
};

void * emit_ast_thread_main(void * opaque)
{
  EmitAstTask * task = static_cast<EmitAstTask *>(opaque);
  try
  {
    task->unit = parse_source_file_ast(*task->srcfile, *task->predefined);
    if(task->bind_mode == BindMode::Types) {
      task->model.reset(new TypesModel());
      DeclBinder binder(*task->model);
      binder.BindTranslationUnit(*task->unit);
    }
    else if(task->bind_mode == BindMode::Semantics) {
      task->model.reset(new TypesModel());
      task->semantics.reset(new SemUnit());
      SemBinder binder(*task->model, *task->semantics);
      binder.BindTranslationUnit(*task->unit);
    }
  }
  catch(const exception & e)
  {
    task->failed = true;
    task->message = e.what();
  }
  return 0;
}

EmitAstTask run_unit_on_large_stack(
    const string & srcfile,
    const vector<pair<string, string>> & predefined,
    BindMode bind_mode)
{
  EmitAstTask task;
  task.srcfile = &srcfile;
  task.predefined = &predefined;
  task.bind_mode = bind_mode;
  task.failed = false;
  pthread_attr_t attributes;
  pthread_t thread;
  if(pthread_attr_init(&attributes) != 0 ||
     pthread_attr_setstacksize(&attributes, cppgm_parse_stack_bytes) != 0 ||
     pthread_create(&thread, &attributes, emit_ast_thread_main, &task) != 0) {
    throw runtime_error("cannot start parse worker thread");
  }
  pthread_join(thread, 0);
  pthread_attr_destroy(&attributes);
  if(task.failed) {
    throw runtime_error(task.message);
  }
  return task;
}

AstDeclPtr parse_ast_on_large_stack(
    const string & srcfile,
    const vector<pair<string, string>> & predefined)
{
  return std::move(run_unit_on_large_stack(srcfile, predefined,
                                           BindMode::ParseOnly).unit);
}

int run_emit_ast_mode(const vector<string> & args)
{
  SourceOutputInvocation invocation =
      parse_source_output_invocation(args, false);

  vector<pair<string, string>> predefined = PredefinedObjectMacros();
  vector<AstDeclPtr> units;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    units.push_back(parse_ast_on_large_stack(invocation.inputs[i],
                                             predefined));
  }

  ofstream out(invocation.outfile.c_str());
  if(!out) {
    throw runtime_error("cannot create output file: " + invocation.outfile);
  }
  PrintAstOutput(units, out);
  return EXIT_SUCCESS;
}

int run_emit_types_mode(const vector<string> & args)
{
  SourceOutputInvocation invocation =
      parse_source_output_invocation(args, false);

  vector<pair<string, string>> predefined = PredefinedObjectMacros();
  vector<unique_ptr<TypesModel>> models;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    EmitAstTask task = run_unit_on_large_stack(invocation.inputs[i],
                                               predefined, BindMode::Types);
    models.push_back(std::move(task.model));
  }

  ofstream out(invocation.outfile.c_str());
  if(!out) {
    throw runtime_error("cannot create output file: " + invocation.outfile);
  }
  out << models.size() << " translation units\n";
  for(size_t i = 0; i < models.size(); ++i) {
    out << "start translation unit " << (i + 1) << "\n";
    PrintTypesOutput(*models[i]->global(), out);
    out << "end translation unit\n";
  }
  return EXIT_SUCCESS;
}

int run_emit_semantics_mode(const vector<string> & args)
{
  SourceOutputInvocation invocation =
      parse_source_output_invocation(args, false);

  vector<pair<string, string>> predefined = PredefinedObjectMacros();
  vector<unique_ptr<SemUnit>> units;
  // The dump's type nodes point at entity records owned by each unit's
  // model, so the models must outlive the printing below.
  vector<unique_ptr<TypesModel>> models;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    EmitAstTask task = run_unit_on_large_stack(invocation.inputs[i],
                                               predefined,
                                               BindMode::Semantics);
    units.push_back(std::move(task.semantics));
    models.push_back(std::move(task.model));
  }

  ofstream out(invocation.outfile.c_str());
  if(!out) {
    throw runtime_error("cannot create output file: " + invocation.outfile);
  }
  out << units.size() << " translation units\n";
  for(size_t i = 0; i < units.size(); ++i) {
    out << "start translation unit " << (i + 1) << "\n";
    PrintSemanticsOutput(*units[i], out);
    out << "end translation unit\n";
  }
  return EXIT_SUCCESS;
}

int run_emit_lowir_mode(const vector<string> & args)
{
  SourceOutputInvocation invocation =
      parse_source_output_invocation(args, true);

  vector<pair<string, string>> predefined = PredefinedObjectMacros();
  vector<unique_ptr<SemUnit>> units;
  // The lowering reads scope identities out of each unit's model and
  // deferred written forms (dependent template arguments) out of each
  // unit's AST, so both must outlive the LowIR emission below.
  vector<unique_ptr<TypesModel>> models;
  vector<AstDeclPtr> asts;
  for(size_t i = 0; i < invocation.inputs.size(); ++i) {
    EmitAstTask task = run_unit_on_large_stack(invocation.inputs[i],
                                               predefined,
                                               BindMode::Semantics);
    units.push_back(std::move(task.semantics));
    models.push_back(std::move(task.model));
    asts.push_back(std::move(task.unit));
  }

  LowerProgram program;
  for(size_t i = 0; i < units.size(); ++i) {
    program.AddUnit(*units[i]);
  }

  ofstream out(invocation.outfile.c_str());
  if(!out) {
    throw runtime_error("cannot create output file: " + invocation.outfile);
  }
  program.Write(out);
  return EXIT_SUCCESS;
}

int run_driver_mode(const vector<string> & args)
{
  const DriverInvocation invocation = parse_driver_invocation(args);
  switch(invocation.mode) {
  case DriverMode::Query:
    return run_unimplemented_mode("driver query mode", "PA34");
  case DriverMode::Preprocess:
    return run_unimplemented_mode("hosted preprocess driver mode (-E)", "PA34");
  case DriverMode::Compile:
    return run_compile_mode(invocation);
  case DriverMode::Link:
    return run_link_mode(invocation);
  }
  throw logic_error("unreachable driver mode");
}

int run_cppgm(const vector<string> & raw_args)
{
  if(has_arg(raw_args, "--batch-stdin")) {
    return run_not_implemented_batch_mode();
  }

  if(has_help_arg(raw_args)) {
    cout << cppgm_help_text();
    return EXIT_SUCCESS;
  }

  vector<string> args = raw_args;
  const EmitMode mode = parse_emit_mode(args);

  switch(mode) {
  case EmitMode::Ast:
    return run_emit_ast_mode(args);
  case EmitMode::Types:
    return run_emit_types_mode(args);
  case EmitMode::Semantics:
    return run_emit_semantics_mode(args);
  case EmitMode::LowIR:
    return run_emit_lowir_mode(args);
  case EmitMode::None:
    return run_driver_mode(args);
  }

  throw logic_error("unreachable emit mode");
}

}  // namespace

int main(int argc, char ** argv)
{
  try
  {
    return run_cppgm(collect_args(argc, argv));
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
