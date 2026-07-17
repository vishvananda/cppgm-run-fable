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
// PA31: the on-disk `-c` artifact is a host ET_REL ELF object. The
// module additionally carries typed host-EH facts per function; the
// ELF writer renders them into .eh_frame/.gcc_except_table, the ELF
// reader parses them back, and the private linker renders them into
// its flat region table. Host sections are the boundary encoding;
// this typed model is the source of truth.

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

// One LSDA action-chain record: a typed catch (type_symbol indexes the
// module symbols), a catch-all, or a cleanup marker. A call site's
// chain lists the actions of every region armed around it,
// innermost-first.
struct EhAction
{
	enum Kind
	{
		EH_CATCH,
		EH_CATCH_ALL,
		EH_CLEANUP
	};

	Kind kind = EH_CATCH;
	long long filter = 0;   // ttype index for catch/catch-all
	int type_symbol = -1;   // module symbol of the _ZTI record
};

// One call-site row with a landing pad. Offsets are relative to the
// function start; rows without landing pads are gaps the LSDA encoder
// fills in.
struct EhCallSite
{
	unsigned long long start = 0;
	unsigned long long length = 0;
	unsigned long long landing_pad = 0;
	int chain = -1;   // index into EhFunctionInfo::chains, -1 for none
};

// Typed host-EH facts of one function: where its code lives, its
// call-site rows and action chains, and the DWARF CFI instruction
// stream describing its frame (present on freshly compiled modules;
// modules re-read from ELF drop it - only object emission needs CFI).
struct EhFunctionInfo
{
	int item = -1;
	unsigned long long item_offset = 0;
	unsigned long long code_size = 0;
	std::vector<EhCallSite> call_sites;
	std::vector<std::vector<EhAction> > chains;
	std::vector<unsigned char> cfi_ops;
};

struct ObjectModule
{
	std::string target;  // normalized backend target name
	std::vector<ImageItem> items;
	std::vector<ObjectSymbol> symbols;
	std::vector<EhFunctionInfo> eh_functions;
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

// Object-like input filenames accepted by link mode (.o / .obj).
bool HasObjectFileName(const std::string & path);

// One link-mode object input: an x86-64 ET_REL ELF relocatable (our
// own -c output or a host-compiled helper object).
ObjectModule LoadObjectModuleFile(const std::string & path,
                                  const std::string & target);

// -l name resolution: lib<name>.o / lib<name>.obj across the -L dirs
// in command order; the first existing candidate wins.
std::string FindLibraryObject(const std::vector<std::string> & lib_dirs,
                              const std::string & name);

}  // namespace toolchain
