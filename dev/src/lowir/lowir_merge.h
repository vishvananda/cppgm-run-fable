#pragma once

#include "lowir/lowir_program.h"

// Merges separately lowered translation units into one program with
// linker-style symbol resolution: vague-linkage (weak) definitions
// dedupe first-wins, a strong definition supersedes weak definitions
// and declares, at most one declare survives per undefined name, and
// internal-binding definitions whose names collide across units are
// renamed apart (unit-scoped names are a presentation tie-breaker, not
// a semantic contract). Throws std::runtime_error when two units both
// define one name strongly. The unit programs are consumed.
LowIRProgram MergeLowIRUnits(vector<LowIRProgram> & units);
