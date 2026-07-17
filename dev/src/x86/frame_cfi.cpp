#include "x86/frame_cfi.h"

#include <stdexcept>

namespace {

const unsigned char DW_CFA_advance_loc = 0x40;
const unsigned char DW_CFA_offset = 0x80;
const unsigned char DW_CFA_advance_loc1 = 0x02;
const unsigned char DW_CFA_advance_loc2 = 0x03;
const unsigned char DW_CFA_advance_loc4 = 0x04;
const unsigned char DW_CFA_def_cfa_offset = 0x0e;
const unsigned char DW_CFA_def_cfa_register = 0x0d;

int DwarfRegister(X64Register reg)
{
	switch (reg)
	{
	case XR_RAX: return 0;
	case XR_RDX: return 1;
	case XR_RCX: return 2;
	case XR_RBX: return 3;
	case XR_RSI: return 4;
	case XR_RDI: return 5;
	case XR_RBP: return 6;
	case XR_RSP: return 7;
	default: return static_cast<int>(reg);   // r8..r15 match
	}
}

void AppendUleb(std::vector<unsigned char> & out, unsigned long long value)
{
	do
	{
		unsigned char byte = static_cast<unsigned char>(value & 0x7f);
		value >>= 7;
		if (value != 0)
			byte |= 0x80;
		out.push_back(byte);
	} while (value != 0);
}

void AppendAdvance(std::vector<unsigned char> & out, std::size_t from,
                   std::size_t to)
{
	std::size_t delta = to - from;
	if (delta == 0)
		return;
	if (delta < 64)
	{
		out.push_back(static_cast<unsigned char>(DW_CFA_advance_loc |
		                                         delta));
	}
	else if (delta < 256)
	{
		out.push_back(DW_CFA_advance_loc1);
		out.push_back(static_cast<unsigned char>(delta));
	}
	else if (delta < 65536)
	{
		out.push_back(DW_CFA_advance_loc2);
		out.push_back(static_cast<unsigned char>(delta));
		out.push_back(static_cast<unsigned char>(delta >> 8));
	}
	else
	{
		out.push_back(DW_CFA_advance_loc4);
		for (int b = 0; b < 4; b++)
			out.push_back(static_cast<unsigned char>(delta >> (8 * b)));
	}
}

}  // namespace

std::vector<unsigned char> BuildFrameCfiOps(const NativeFunctionEh & fn)
{
	std::vector<unsigned char> out;
	AppendAdvance(out, 0, fn.prologue_push_end);
	out.push_back(DW_CFA_def_cfa_offset);
	AppendUleb(out, 16);
	out.push_back(static_cast<unsigned char>(DW_CFA_offset | 6));   // rbp
	AppendUleb(out, 2);   // CFA-16, data alignment -8
	AppendAdvance(out, fn.prologue_push_end, fn.prologue_frame_end);
	out.push_back(DW_CFA_def_cfa_register);
	AppendUleb(out, 6);   // rbp
	AppendAdvance(out, fn.prologue_frame_end, fn.prologue_end);
	for (std::size_t i = 0; i < fn.saved_regs.size(); i++)
	{
		long long slot = fn.saved_regs[i].second;   // rbp-relative
		long long cfa_relative = slot - 16;
		if (cfa_relative >= 0 || cfa_relative % 8 != 0)
			throw std::logic_error(
				"callee-save slot is not an 8-aligned CFA offset");
		out.push_back(static_cast<unsigned char>(
			DW_CFA_offset | DwarfRegister(fn.saved_regs[i].first)));
		AppendUleb(out, static_cast<unsigned long long>(-cfa_relative / 8));
	}
	return out;
}
