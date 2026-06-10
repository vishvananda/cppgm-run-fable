#pragma once

#include "lowir/lowir_program.h"

// PA13 stack frame layout. Every parameter, slot, and temporary gets a
// bp-relative home below the saved bp, allocated in this order:
//
//   1. one 8-byte synthetic home for the hidden result pointer when the
//      return type is f80 or obj<...>
//   2. parameter homes in declaration order
//   3. slot homes in declaration order
//   4. temporary homes in first-definition order
//   5. a 64-byte scratch tail of four 16-byte f80 slots whenever the
//      function converts scalars or touches f80 anywhere
//
// Scalars own 8 bytes; f80 owns 16; obj<NxA> rounds N up to 8 bytes.
// `offset` is the positive bp displacement of the home's lowest byte, so
// the home is addressed as [bp-offset] (and [bp-offset+8] for the high
// word of 16-byte homes).
struct LowIRHome
{
	long offset = 0;
	LowIRType type;
};

struct LowIRFrame
{
	map<string, LowIRHome> values;   // params, slots, and temps by name
	long ret_pointer_offset = 0;     // 0 when the return is direct
	long homes_size = 0;
	bool has_scratch = false;
	long frame_size = 0;

	// 16-byte scratch slots S1..S4 used by f80 and conversion templates.
	long scratch(int index) const { return homes_size + 16 * index; }

	const LowIRHome & home(const string & name) const;
};

LowIRFrame ComputeLowIRFrame(const LowIRFunction & function);
