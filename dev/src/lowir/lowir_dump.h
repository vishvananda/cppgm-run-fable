#pragma once

#include <ostream>

#include "lowir/lowir_program.h"

// PA37 canonical LowIR presentation: the deterministic text form shared
// by `lowiropt` output and the driver's optimized `--emit-lowir` output.
// Top-level entries print in the pa13/lowir.md phase order (declare
// global, declare function, global, function, alias object) with input
// order preserved inside each phase; parsed spellings (literals,
// metadata items, operand names) are preserved verbatim.

// The spelling of one LowIR type ("i64", "obj<16x8>", ...).
string LowIRTypeText(const LowIRType & type);

// The spelling of one operand ("%t", "$slot", "@g", "-1", "nullptr").
string LowIROperandText(const LowIROperand & operand);

// One instruction line without indentation or trailing newline.
string LowIRInstructionText(const LowIRInstruction & ins);

// Writes the canonical program dump.
void DumpLowIRProgram(const LowIRProgram & program, std::ostream & out);
