#pragma once

#include <string>

#include "toolchain/object_module.h"

// PA29 host-object interoperability: reads one x86-64 ET_REL ELF64
// relocatable file (a host-compiled helper object named by -l) into
// the linker's module shape. Allocatable sections become items,
// symbols keep their section offsets, and RELA entries become patches
// against module symbol indices. Unwind tables and notes are dropped;
// GOT-relative references synthesize module-local GOT slots.

namespace toolchain {

ObjectModule ParseElfObjectBytes(const std::string & bytes,
                                 const std::string & input_name,
                                 const std::string & target);

}  // namespace toolchain
