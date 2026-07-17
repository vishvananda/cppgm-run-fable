#pragma once

#include <string>

#include "toolchain/object_module.h"

// PA29 freestanding runtime library: the C++ translation unit that
// backs the compiler's runtime references (Itanium EH entry points,
// RTTI matching, __dynamic_cast, operator new/delete) without host
// libc. The driver compiles it through the ordinary per-unit pipeline
// when a link leaves runtime names unresolved, so it flows through the
// same code paths as user code.

namespace toolchain {

const std::string & RuntimeLibrarySource();
const char * RuntimeLibraryName();

// PA31 runtime-owned typeinfo: libstdc++ exports the typeinfo of
// fundamental types and their pointer / pointer-to-const variants, so
// host objects must reference those records instead of defining their
// own. Object emission demotes matching weak definitions to undefined
// references; the private runtime module defines the same set.
bool IsRuntimeOwnedTypeinfoName(const std::string & name);
void AppendRuntimeTypeinfo(ObjectModule & module);

}  // namespace toolchain
