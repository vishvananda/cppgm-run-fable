#pragma once

#include <string>
#include <vector>

#include "x86/elf_program.h"

// PA29 compiler object model: one relocatable module. Items are the
// encoded image pieces (function code, global data, pool constants,
// host ELF sections); patches inside items reference symbols by dense
// symbol index (the encoder's label ids). Symbols carry the module-
// local low name and the cross-module external name; only strong/weak
// definitions and undefined references participate in link-time
// resolution - internal symbols are module-private.
//
// The on-disk encoding is an internal cppgm++ contract (PA31/PA32
// replace it with host-linker-compatible objects).

namespace toolchain {

struct ObjectSymbol
{
	enum Binding
	{
		SB_UNDEFINED,
		SB_STRONG,
		SB_WEAK,
		SB_INTERNAL
	};

	std::string low_name;       // module-local spelling ("" for aliases)
	std::string external_name;  // resolution key ("" for module-private)
	Binding binding = SB_UNDEFINED;
	int item = -1;              // defining item index, -1 when undefined
	long long offset = 0;       // symbol offset inside the item
};

struct ObjectModule
{
	std::string target;  // normalized backend target name
	std::vector<ImageItem> items;
	std::vector<ObjectSymbol> symbols;
	int entry_symbol = -1;  // role=entry definition
	int init_symbol = -1;   // role=init definition
	int fini_symbol = -1;   // role=fini definition
};

// Backend target names: "linux" and the matching x86_64 triple map to
// the canonical backend spelling; anything else is rejected.
std::string NormalizeTargetName(const std::string & target);

// Whole-file reads and input classification by content magic.
std::string ReadFileBytes(const std::string & path);
bool IsElfObjectBytes(const std::string & bytes);
bool IsCppgmObjectBytes(const std::string & bytes);

// Object-like input filenames accepted by link mode (.o / .obj).
bool HasObjectFileName(const std::string & path);

// One link-mode object input, classified by content: cppgm compiler
// objects and host ELF relocatables are both accepted.
ObjectModule LoadObjectModuleFile(const std::string & path,
                                  const std::string & target);

// -l name resolution: lib<name>.o / lib<name>.obj across the -L dirs
// in command order; the first existing candidate wins.
std::string FindLibraryObject(const std::vector<std::string> & lib_dirs,
                              const std::string & name);

void WriteObjectModuleFile(const std::string & path,
                           const ObjectModule & module);
ObjectModule ParseObjectModuleBytes(const std::string & bytes,
                                    const std::string & input_name);

}  // namespace toolchain
