#pragma once

#include "lowir/lowir_program.h"

// PA37 LowIR optimization pipeline. Level 0 is a no-op (the canonical
// parse/dump round trip happens in the dumper); level 1 applies the
// semantic-preserving local/CFG/inline pass families; level 2 adds
// conservative non-escaping slot promotion. The pipeline mutates the
// parsed program in place; serialization back through lowir_dump.h
// yields the canonical optimized text.
void OptimizeLowIRProgram(LowIRProgram & program, int level);
