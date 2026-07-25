#pragma once

#include <string>
#include <vector>

#include "toolchain/hosted_env.h"
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
	// PA34: driver compiles run against the hosted environment (host
	// predefined macros, system include chain, -D/-U/-include/-isystem).
	// The built-in runtime library TU keeps the unhosted environment.
	bool hosted = false;
	HostedPreprocessConfig preprocess;
};

ObjectModule CompileSourceFileToModule(const std::string & path,
                                       const CompileOptions & options);

// PA37: the serialized LowIR text of one translation unit exactly as
// the object path lowers it (hosted separate-compilation mode). The
// driver's optimizing --emit-lowir surface parses, optimizes, and
// re-serializes this text so the LowIR file remains the only channel
// between the frontend and object preparation.
std::string LowerSourceFileToLowIRText(const std::string & path,
                                       const CompileOptions & options);

// The built-in runtime library compiles from in-memory text through
// the identical pipeline.
ObjectModule CompileSourceTextToModule(const std::string & presumed_name,
                                       const std::string & text,
                                       const CompileOptions & options);

// PA37 LowIR object input: serialized LowIR text compiles through the
// same optimize-then-object pipeline the source path uses after its
// own lowering, so the object built from a `--emit-lowir -g0 -O0`
// file matches the direct source compile byte for byte.
ObjectModule CompileLowIRTextToModule(const std::string & text,
                                      const CompileOptions & options);

// Whether `text` looks like serialized LowIR (its first token line is
// a LowIR top-level keyword) rather than C++ source.
bool LooksLikeLowIRText(const std::string & text);

}  // namespace toolchain
