#pragma once

#include <string>
#include <vector>

#include "mir_model.h"
#include "x86/elf_program.h"

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

// PA29 relocatable encoding: the same per-function/per-global encoding
// with no startup sled and no image layout. Items are functions, then
// globals, then pool constants; patches stay keyed by dense label ids.
// Labels of names not defined in this program are external references
// (label_items -1) the linker resolves by symbol name.
struct NativeModule
{
	std::vector<ImageItem> items;
	std::vector<std::string> label_names;  // label id -> name ("" = pool)
	std::vector<int> label_items;          // label id -> item index or -1
};

NativeModule EncodeMirProgramModule(const mir_model::MirProgram & program);
