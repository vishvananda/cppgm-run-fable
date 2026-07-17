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

// PA31 typed host-EH facts recorded while encoding one function: the
// byte ranges of region-annotated call sites, each region's landing
// pad offset and action clauses, and the frame shape the DWARF CFI
// builder needs. Object emission renders these into .eh_frame /
// .gcc_except_table; the private linker renders them into its flat
// region table.
struct NativeEhCallSite
{
	std::size_t start = 0;
	std::size_t end = 0;
	int region = -1;
};

struct NativeEhRegion
{
	std::size_t landing_offset = 0;
	bool cleanup = false;
	int parent = -1;
	std::vector<mir_model::HostEhClause> clauses;
};

struct NativeFunctionEh
{
	int item = -1;                     // module item index of the code
	std::string name;
	std::size_t code_size = 0;
	std::vector<NativeEhRegion> regions;
	std::vector<NativeEhCallSite> call_sites;
	// frame facts for CFI: prologue advance points and callee-save
	// spill slots (rbp-relative offsets; CFA is rbp+16 after the
	// prologue)
	std::size_t prologue_push_end = 0;   // after `push rbp`
	std::size_t prologue_frame_end = 0;  // after `mov rbp, rsp`
	std::size_t prologue_end = 0;        // after saves/rsp adjustment
	std::size_t stack_size = 0;
	std::vector<std::pair<X64Register, long long> > saved_regs;
};

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
	std::vector<NativeFunctionEh> function_eh;  // one per function item
};

NativeModule EncodeMirProgramModule(const mir_model::MirProgram & program);
