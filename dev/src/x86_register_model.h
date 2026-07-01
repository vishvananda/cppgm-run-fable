#pragma once

// Register and condition-code names used by the optional MIR model scaffold.
// These enums are global so the scaffold matches the rest of the starter
// backend code without requiring an adapter layer.

enum X64Register
{
  XR_RAX = 0,
  XR_RCX = 1,
  XR_RDX = 2,
  XR_RBX = 3,
  XR_RSP = 4,
  XR_RBP = 5,
  XR_RSI = 6,
  XR_RDI = 7,
  XR_R8 = 8,
  XR_R9 = 9,
  XR_R10 = 10,
  XR_R11 = 11,
  XR_R12 = 12,
  XR_R13 = 13,
  XR_R14 = 14,
  XR_R15 = 15
};

enum XmmRegister
{
  XMM_0 = 0,
  XMM_1 = 1,
  XMM_2 = 2,
  XMM_3 = 3,
  XMM_4 = 4,
  XMM_5 = 5,
  XMM_6 = 6,
  XMM_7 = 7
};

enum X86Condition
{
  XC_O  = 0x0,
  XC_NO = 0x1,
  XC_B  = 0x2,
  XC_AE = 0x3,
  XC_E  = 0x4,
  XC_NE = 0x5,
  XC_BE = 0x6,
  XC_A  = 0x7,
  XC_S  = 0x8,
  XC_NS = 0x9,
  XC_P  = 0xA,
  XC_NP = 0xB,
  XC_L  = 0xC,
  XC_GE = 0xD,
  XC_LE = 0xE,
  XC_G  = 0xF
};
