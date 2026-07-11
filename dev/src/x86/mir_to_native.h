#pragma once

#include <string>

#include "mir_model.h"

// PA28 native emission: encode a typed machine-IR program into x86-64
// machine code and write a static Linux ELF executable through the
// PA9 image writer (one RWX PT_LOAD at 0x400000).
//
// Image layout: item 0 is the startup sled (the entry point), then
// one code item per function, one data item per global, and finally
// the literal-pool data items materialized while encoding. Each
// function is a single image item; block labels resolve to intra-item
// offsets that the encoder back-patches itself once the whole
// function is encoded (all encodings are length-stable), so only
// functions, globals, and pool constants carry image labels.
void WriteMirProgramExecutable(const mir_model::MirProgram & program,
                               const std::string & path);
