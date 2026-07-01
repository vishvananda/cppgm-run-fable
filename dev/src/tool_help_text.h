#pragma once

inline const char * cppgm_help_text()
{
  return
      "usage: cppgm++ --emit-ast -o <outfile> <srcfile>...\n"
      "   or: cppgm++ --emit-types -o <outfile> <srcfile>...\n"
      "   or: cppgm++ --emit-semantics -o <outfile> <srcfile>...\n"
      "   or: cppgm++ --emit-lowir -o <outfile> [--witness <logfile>] [--witness-debug <logfile>] <srcfile>...\n"
      "   or: cppgm++ --emit-abi-facts -o <outfile> <srcfile>...\n"
      "   or: cppgm++ -c [options] <srcfile-or-lowirfile>...\n"
      "   or: cppgm++ -E [options] <srcfile>...\n"
      "   or: cppgm++ [options] <input>...\n"
      "\n"
      "query flags:\n"
      "  --help, -h\n"
      "  --version\n"
      "  -v\n"
      "  -dumpmachine\n"
      "  -dumpversion\n"
      "  -print-search-dirs\n"
      "\n"
      "driver flags:\n"
      "  -E                  preprocess only\n"
      "  -c                  compile only\n"
      "  -o <outfile>        explicit output path\n"
      "  -I <dir> / -I<dir>  user include search path\n"
      "  -isystem <dir>      system include search path\n"
      "  -D <macro> / -D<macro>\n"
      "  -U <macro> / -U<macro>\n"
      "  -include <file>\n"
      "  -L <dir> / -L<dir>\n"
      "  -l <name> / -l<name>\n"
      "  -O0 / -O1 / -O2\n"
      "  -stdlib <name> / -stdlib=<name>\n"
      "  --witness <logfile>\n"
      "  --witness-debug <logfile>\n"
      "  --target <target>\n";
}

inline const char * lowir2cy86_help_text()
{
  return
      "usage: lowir2cy86 -o <outfile> <lowirfile>...\n"
      "\n"
      "query flags:\n"
      "  --help, -h\n"
      "\n"
      "required flags:\n"
      "  -o <outfile>\n";
}

inline const char * lowir2native_help_text()
{
  return
      "usage: lowir2native -o <outfile> <lowirfile>...\n"
      "   or: lowir2native -O0 -o <outfile> <lowirfile>...\n"
      "   or: lowir2native -O1 -o <outfile> <lowirfile>...\n"
      "   or: lowir2native -O2 -o <outfile> <lowirfile>...\n"
      "   or: lowir2native --dump-machine-ir <mirfile> <lowirfile>...\n"
      "   or: lowir2native -o <outfile> --dump-machine-ir <mirfile> <lowirfile>...\n"
      "\n"
      "query flags:\n"
      "  --help, -h\n"
      "\n"
      "driver flags:\n"
      "  -O0 / -O1 / -O2\n"
      "  -o <outfile>\n"
      "  --target <target>\n"
      "  --dump-machine-ir <mirfile>\n"
      "  --dump-native-plan <mirfile>\n";
}

inline const char * lowiropt_help_text()
{
  return
      "usage: lowiropt -O0 -o <outfile> <lowirfile>...\n"
      "   or: lowiropt -O1 -o <outfile> <lowirfile>...\n"
      "   or: lowiropt -O2 -o <outfile> <lowirfile>...\n"
      "\n"
      "query flags:\n"
      "  --help, -h\n"
      "\n"
      "required flags:\n"
      "  -O0 / -O1 / -O2\n"
      "  -o <outfile>\n";
}
