#pragma once

#include <string>
#include <vector>

#include "toolchain/object_module.h"

// PA29 per-translation-unit compilation: one C++ source runs the full
// PA10-PA12 front end, lowers alone through LowerProgram in separate-
// compilation mode, round-trips the canonical LowIR text through the
// parser and validator (no entry requirement), lowers to machine IR,
// and encodes into one relocatable object module. Link mode and `-c`
// share this path, so direct source linking behaves exactly like
// explicit compile-then-link.

namespace toolchain {

struct CompileOptions
{
	std::vector<std::string> include_dirs;  // -I, in command order
	std::string target;                     // normalized backend target
	int optimize = 0;                       // -O<n> level
};

ObjectModule CompileSourceFileToModule(const std::string & path,
                                       const CompileOptions & options);

// The built-in runtime library compiles from in-memory text through
// the identical pipeline.
ObjectModule CompileSourceTextToModule(const std::string & presumed_name,
                                       const std::string & text,
                                       const CompileOptions & options);

}  // namespace toolchain
