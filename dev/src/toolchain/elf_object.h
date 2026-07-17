#pragma once

#include <string>

#include "toolchain/object_module.h"

// PA31 host object emission: renders one compiled module into an
// x86-64 ET_REL ELF relocatable the host linker consumes directly.
// Code items become .text, data items .data, and the typed host-EH
// facts become real .eh_frame CFI (one FDE per function, personality
// and LSDA references only for landing-pad functions) plus
// .gcc_except_table LSDA bytes with DW.ref-style indirection slots
// for the catch type table. Program roles are encoded in standard
// form: `main` stays a global function symbol and init/fini hooks
// become .init_array/.fini_array entries.

namespace toolchain {

void WriteElfObjectFile(const std::string & path,
                        const ObjectModule & module);

}  // namespace toolchain
