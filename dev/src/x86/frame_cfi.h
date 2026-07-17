#pragma once

#include <vector>

#include "x86/mir_to_native.h"

// PA31 DWARF call-frame information for one encoded function: the
// CFA instruction stream an .eh_frame FDE carries. The generated
// prologue shape is fixed (push rbp; mov rbp, rsp; sub rsp, N;
// callee-save spills into 8-aligned rbp-relative slots), so the
// program is: advance past the push (CFA rsp+16, rbp at CFA-16),
// advance past the frame move (CFA register rbp), then the
// callee-save offset rules at the end of the prologue.
//
// The stream assumes the standard x86-64 CIE header: CFA rsp+8,
// return address (r16) at CFA-8, code alignment 1, data alignment -8.
std::vector<unsigned char> BuildFrameCfiOps(const NativeFunctionEh & fn);
