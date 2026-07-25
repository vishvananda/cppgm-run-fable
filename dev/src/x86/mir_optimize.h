#pragma once

#include "mir_model.h"

// PA38 machine-backend optimization levels. Level 0 is a no-op (the
// PA28 baseline). Level 1 performs local machine-IR cleanup: frame
// address folding, block-local copy coalescing and immediate
// rematerialization, dead-copy elimination, zero-compare branch
// rewriting, and branch-tail/fallthrough collapse. Level 2 adds
// whole-function work: jump-trace block layout, callee-saved
// preservation pruning, and stack reservation recompute.
//
// Both lowir2native and the cppgm++ object/executable paths call this
// after LowerLowIRProgramToMir; the optimized program feeds the MIR
// dump and native emission alike.

namespace mir_optimize {

void OptimizeMirProgram(mir_model::MirProgram & program, int level);

}  // namespace mir_optimize
