#pragma once

#include <string>
#include <vector>

#include "toolchain/object_module.h"

// PA29 link mode: resolve external names across relocatable modules,
// synthesize the program startup (every unit's role=init hook in link
// order, the entry, then the role=fini hooks in reverse), and lay the
// items out into one native executable through the PA9 image writer.
//
// The linker owns all symbol-resolution policy, including when the
// built-in runtime library joins the link: only when names remain
// unresolved after the normal inputs and the synthesized support
// module. It stays ignorant of the front end by receiving the
// runtime module through a supplier the driver wires to the ordinary
// compile pipeline.

namespace toolchain {

struct LinkInput
{
	std::string name;  // input spelling for diagnostics
	ObjectModule module;
};

typedef LinkInput (*RuntimeModuleSupplier)(const std::string & target);

// Takes the inputs by value: the link rewrites hook symbols and patch
// labels in place, so callers hand their modules over (move them in).
void LinkExecutable(std::vector<LinkInput> inputs,
                    const std::string & outfile,
                    const std::string & target,
                    RuntimeModuleSupplier runtime_supplier);

}  // namespace toolchain
