#pragma once

#include <string>

// PA29 freestanding runtime library: the C++ translation unit that
// backs the compiler's runtime references (Itanium EH entry points,
// RTTI matching, __dynamic_cast, operator new/delete) without host
// libc. The driver compiles it through the ordinary per-unit pipeline
// when a link leaves runtime names unresolved, so it flows through the
// same code paths as user code.

namespace toolchain {

const std::string & RuntimeLibrarySource();
const char * RuntimeLibraryName();

}  // namespace toolchain
