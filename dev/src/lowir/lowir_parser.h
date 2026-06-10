#pragma once

#include "lowir/lowir_program.h"

// PA13 LowIR parser: matches the token stream of the concatenated
// translation units against pa13.gram and builds the LowIR object model.
// All grammar violations throw std::runtime_error; the lowir2cy86 driver
// reports them as EXIT_FAILURE.
LowIRProgram ParseLowIRProgram(const string & text);
