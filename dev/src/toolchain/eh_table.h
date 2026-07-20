#pragma once

#include <vector>

#include "toolchain/object_module.h"

// PA31 LSDA (.gcc_except_table) encoding: typed per-function host-EH
// facts <-> Itanium language-specific data area bytes. The encoder and
// decoder share this file so the ELF writer and the ELF reader can
// never disagree about the format.
//
// Encoding profile (the basic course subset):
// - LPStart omitted (0xff): landing pads are function-relative.
// - TType encoding 0x9b (indirect pcrel sdata4) when catches exist,
//   omitted (0xff) for cleanup-only tables. Each ttype entry is a
//   4-byte pc-relative reference to an 8-byte indirection slot that
//   holds the absolute _ZTI address (DW.ref.* style).
// - Call sites use uleb128 encoding (0x01) and cover [0, code_size):
//   gaps get landing pad 0; a row whose chain carries no catch
//   records gets action 0 (pure cleanup).
// - Action records are {sleb filter, sleb next}: catches carry their
//   positive selector filter, cleanups filter 0, chains list the
//   armed regions' actions innermost-first.
// - PA36 exception-spec records carry the negative filter -(offset+1)
//   into the spec area that trails the ttype table: null-terminated
//   uleb runs of the spec's allowed-type ttype indices.

namespace toolchain {

// One ttype table entry position. On encode, `value` is the module
// symbol whose indirection slot the 4-byte pc-relative field at
// `lsda_offset` must reference (null catch-all entries are omitted).
// On decode, `value` is the entry's positive filter number; the
// caller resolves the relocation at `lsda_offset` back to a symbol
// and rewrites the matching chain actions.
struct EhTypeRef
{
	unsigned long long lsda_offset = 0;
	int value = -1;
};

struct EhTableBytes
{
	std::vector<unsigned char> bytes;
	std::vector<EhTypeRef> type_refs;
};

// Encodes one function's typed facts. Throws on facts the profile
// cannot represent (overlapping rows, filters out of sleb range).
EhTableBytes EncodeEhTable(const EhFunctionInfo & eh);

// Decodes one LSDA back into typed facts (call sites, chains). The
// caller resolves `type_refs` back to symbols and fills item/offset
// facts. Returns false when the bytes do not match the profile.
bool DecodeEhTable(const std::vector<unsigned char> & bytes,
                   unsigned long long lsda_offset,
                   EhFunctionInfo & eh,
                   std::vector<EhTypeRef> & type_refs);

}  // namespace toolchain
