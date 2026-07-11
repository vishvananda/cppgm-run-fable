#pragma once

#include <string>
#include <vector>

#include "toolchain/object_module.h"

// PA29 link mode: resolve external names across relocatable modules,
// synthesize the program startup (every unit's role=init hook in link
// order, the entry, then the role=fini hooks in reverse), and lay the
// items out into one native executable through the PA9 image writer.
//
// The linker is name-driven and knows nothing about the front end; the
// driver decides which modules to link (including the on-demand
// runtime-library module) using UnresolvedExternals.

namespace toolchain {

struct LinkInput
{
	std::string name;  // input spelling for diagnostics
	ObjectModule module;
};

// External names referenced by patches in `inputs` with no strong or
// weak definition anywhere in `inputs` (deduplicated, in first-use
// order). Duplicate-definition errors are NOT raised here; they
// surface in LinkExecutable.
std::vector<std::string> UnresolvedExternals(
	const std::vector<LinkInput> & inputs);

void LinkExecutable(const std::vector<LinkInput> & inputs,
                    const std::string & outfile,
                    const std::string & target);

}  // namespace toolchain
