#pragma once

#include "lowir/lowir_program.h"

// PA37 LowIR optimization pipeline. Level 0 is a no-op (the canonical
// parse/dump round trip happens in the dumper); level 1 applies the
// semantic-preserving local/CFG/inline pass families; level 2 adds
// conservative non-escaping slot promotion. The pipeline mutates the
// parsed program in place; serialization back through lowir_dump.h
// yields the canonical optimized text.
void OptimizeLowIRProgram(LowIRProgram & program, int level);

// Drops function/global declarations nothing in the program references
// (no role= hooks, no tls_for wrappers). The driver emit surface uses
// this so demand-registered but unused hosted declarations do not leak
// into the serialized unit; `lowiropt` itself keeps its input's
// declaration list.
void PruneUnreferencedLowIRDeclares(LowIRProgram & program);

// Object preparation's vague-linkage discard: weak function
// definitions nothing in the unit references drop out of the compiled
// module together with their aliases. At level 0 only
// trivial_lifecycle=yes wrappers drop (the spec'd use of that fact);
// at level 1+ every unreferenced weak function does, matching the
// host compiler's optimized objects. The serialized LowIR keeps the
// definitions; only object preparation calls this.
void RemoveUnreferencedWeakFunctions(LowIRProgram & program, int level);
