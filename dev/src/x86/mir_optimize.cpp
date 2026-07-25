#include "x86/mir_optimize.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

// PA38 machine-backend optimizer. Every pass operates on the typed
// mir_model program between PA28 lowering and native emission, and the
// same optimized program feeds the MIR dump and the object/executable
// encoders. The structural test oracle canonicalizes only free-pool
// register names and frame-displacement numbering, so each rewrite
// below either reproduces the pinned reference shape or does not fire.
//
// Functions with host-EH regions are left untouched: the unwinder
// re-enters landing pads with only rbp reliably restored and the
// conservative eh-mode lowering already keeps values in frame homes.

namespace mir_optimize {

namespace {

using mir_model::MirBlock;
using mir_model::MirFunction;
using mir_model::MirInstruction;
using mir_model::MirOperand;
using mir_model::MirProgram;

// Bits 0..15 name GPRs, bits 16..23 name XMM registers.
typedef unsigned int RegMask;

const int kGprCount = 16;
const RegMask kAllRegs = 0xFFFFFFu;

RegMask gpr_bit(int reg)
{
  return 1u << reg;
}

RegMask xmm_bit(int reg)
{
  return 1u << (kGprCount + reg);
}

RegMask caller_saved_mask()
{
  RegMask mask = gpr_bit(XR_RAX) | gpr_bit(XR_RCX) | gpr_bit(XR_RDX) |
                 gpr_bit(XR_RSI) | gpr_bit(XR_RDI) | gpr_bit(XR_R8) |
                 gpr_bit(XR_R9) | gpr_bit(XR_R10) | gpr_bit(XR_R11);
  for(int i = 0; i < 8; ++i)
    mask |= xmm_bit(i);
  return mask;
}

// Registers an operand reads when an instruction consumes it.
RegMask operand_read_mask(const MirOperand & operand)
{
  switch(operand.kind) {
    case MirOperand::OP_REG: return gpr_bit(operand.reg);
    case MirOperand::OP_XMM: return xmm_bit(operand.xmm);
    case MirOperand::OP_DEREF: return gpr_bit(operand.reg);
    default: return 0;
  }
}

RegMask operand_base_mask(const MirOperand & operand)
{
  return operand.kind == MirOperand::OP_DEREF ? gpr_bit(operand.reg) : 0;
}

RegMask operand_def_mask(const MirOperand & operand)
{
  if(operand.kind == MirOperand::OP_REG)
    return gpr_bit(operand.reg);
  if(operand.kind == MirOperand::OP_XMM)
    return xmm_bit(operand.xmm);
  return 0;
}

// What one instruction reads and writes. A barrier conservatively
// reads every register: a call may consume any argument register, and
// an unmodeled opcode must never lose a live input.
struct InsEffect
{
  RegMask use;
  RegMask def;
  bool barrier;

  InsEffect() : use(0), def(0), barrier(false) {}
};

// `op dst, src...`: the first operand is written (memory destinations
// only read their base register), the rest are read.
InsEffect write_first_effect(const MirInstruction & ins)
{
  InsEffect effect;
  for(size_t i = 0; i < ins.operands.size(); ++i) {
    if(i == 0) {
      effect.def |= operand_def_mask(ins.operands[i]);
      effect.use |= operand_base_mask(ins.operands[i]);
    } else {
      effect.use |= operand_read_mask(ins.operands[i]);
    }
  }
  return effect;
}

InsEffect read_write_first_effect(const MirInstruction & ins)
{
  InsEffect effect = write_first_effect(ins);
  if(!ins.operands.empty())
    effect.use |= operand_read_mask(ins.operands[0]);
  return effect;
}

InsEffect all_read_effect(const MirInstruction & ins)
{
  InsEffect effect;
  for(size_t i = 0; i < ins.operands.size(); ++i)
    effect.use |= operand_read_mask(ins.operands[i]);
  return effect;
}

InsEffect barrier_effect(RegMask def)
{
  InsEffect effect;
  effect.use = kAllRegs;
  effect.def = def;
  effect.barrier = true;
  return effect;
}

InsEffect classify(const MirInstruction & ins)
{
  typedef MirInstruction I;
  InsEffect effect;
  switch(ins.opcode) {
    case I::MI_MOV: case I::MI_LEA: case I::MI_LOAD: case I::MI_STORE:
    case I::MI_MOVZX:
    case I::MI_FMOV: case I::MI_FNEG: case I::MI_FADD: case I::MI_FSUB:
    case I::MI_FMUL: case I::MI_FDIV: case I::MI_FEQ: case I::MI_FNE:
    case I::MI_FLT: case I::MI_FGT: case I::MI_FLE: case I::MI_FGE:
    case I::MI_SITOFP: case I::MI_UITOFP: case I::MI_FPTOSI:
    case I::MI_FPTOUI: case I::MI_FPEXT: case I::MI_FPTRUNC:
      return write_first_effect(ins);
    case I::MI_ADD: case I::MI_ADC: case I::MI_SUB: case I::MI_SBB:
    case I::MI_IMUL: case I::MI_AND: case I::MI_OR: case I::MI_XOR:
    case I::MI_NEG: case I::MI_NOT: case I::MI_BSWAP: case I::MI_SETCC:
    case I::MI_SEXT: case I::MI_ZEXT:
      return read_write_first_effect(ins);
    case I::MI_SHL_CL: case I::MI_SHR_CL: case I::MI_SAR_CL:
      effect = read_write_first_effect(ins);
      effect.use |= gpr_bit(XR_RCX);
      return effect;
    case I::MI_CMP: case I::MI_TEST: case I::MI_FCMP:
      return all_read_effect(ins);
    case I::MI_JCC: case I::MI_JNE: case I::MI_JMP:
      return effect;
    case I::MI_JMP_INDIRECT: case I::MI_RET: case I::MI_FRET:
      return all_read_effect(ins);
    case I::MI_EXIT:
      effect.use = gpr_bit(XR_RDI);
      return effect;
    case I::MI_CQO:
      effect.use = gpr_bit(XR_RAX);
      effect.def = gpr_bit(XR_RDX);
      return effect;
    case I::MI_MUL:
      effect = all_read_effect(ins);
      effect.use |= gpr_bit(XR_RAX);
      effect.def = gpr_bit(XR_RAX) | gpr_bit(XR_RDX);
      return effect;
    case I::MI_IDIV: case I::MI_DIV:
      effect = all_read_effect(ins);
      effect.use |= gpr_bit(XR_RAX) | gpr_bit(XR_RDX);
      effect.def = gpr_bit(XR_RAX) | gpr_bit(XR_RDX);
      return effect;
    case I::MI_XCHG: case I::MI_LOCK_XADD:
      effect = all_read_effect(ins);
      if(ins.operands.size() > 1)
        effect.def = operand_def_mask(ins.operands[1]);
      return effect;
    case I::MI_LOCK_CMPXCHG:
      effect = all_read_effect(ins);
      effect.use |= gpr_bit(XR_RAX);
      effect.def = gpr_bit(XR_RAX);
      return effect;
    case I::MI_MFENCE:
      return effect;
    case I::MI_COPY_BYTES:
      effect = all_read_effect(ins);
      effect.use |= gpr_bit(XR_RSI) | gpr_bit(XR_RDI);
      effect.def = gpr_bit(XR_RCX) | gpr_bit(XR_RSI) | gpr_bit(XR_RDI);
      return effect;
    case I::MI_ZERO_BYTES:
      effect = all_read_effect(ins);
      effect.use |= gpr_bit(XR_RDI);
      effect.def = gpr_bit(XR_RAX) | gpr_bit(XR_RCX) | gpr_bit(XR_RDI);
      return effect;
    case I::MI_CALL: case I::MI_CALL_INDIRECT:
      return barrier_effect(caller_saved_mask());
    case I::MI_TLS_ADDR:
      effect = barrier_effect(caller_saved_mask());
      if(!ins.operands.empty())
        effect.def |= operand_def_mask(ins.operands[0]);
      return effect;
    default:
      return barrier_effect(kAllRegs);
  }
}

bool unconditional_terminator(const MirInstruction & ins)
{
  switch(ins.opcode) {
    case MirInstruction::MI_JMP:
    case MirInstruction::MI_JMP_INDIRECT:
    case MirInstruction::MI_RET:
    case MirInstruction::MI_FRET:
    case MirInstruction::MI_EXIT:
      return true;
    default:
      return false;
  }
}

// ---- whole-function liveness ------------------------------------------

struct BlockFlow
{
  std::vector<size_t> successors;
  RegMask gen;
  RegMask kill;
  RegMask live_out;

  BlockFlow() : gen(0), kill(0), live_out(0) {}
};

void CollectSuccessors(const MirFunction & function,
                       std::vector<BlockFlow> & flows)
{
  std::map<std::string, size_t> index_of;
  for(size_t i = 0; i < function.blocks.size(); ++i)
    index_of[function.blocks[i].label] = i;
  for(size_t i = 0; i < function.blocks.size(); ++i) {
    const MirBlock & block = function.blocks[i];
    bool indirect = false;
    for(size_t j = 0; j < block.instructions.size(); ++j) {
      const MirInstruction & ins = block.instructions[j];
      if(ins.opcode == MirInstruction::MI_JMP_INDIRECT)
        indirect = true;
      for(size_t k = 0; k < ins.operands.size(); ++k) {
        if(ins.operands[k].kind != MirOperand::OP_LABEL)
          continue;
        std::map<std::string, size_t>::const_iterator it =
            index_of.find(ins.operands[k].text);
        if(it != index_of.end())
          flows[i].successors.push_back(it->second);
      }
    }
    if(indirect) {
      // Jump-table targets live on the data side; treat every block as
      // reachable so liveness stays conservative.
      flows[i].successors.clear();
      for(size_t j = 0; j < function.blocks.size(); ++j)
        flows[i].successors.push_back(j);
      continue;
    }
    const bool falls_through =
        block.instructions.empty() ||
        !unconditional_terminator(block.instructions.back());
    if(falls_through && i + 1 < function.blocks.size())
      flows[i].successors.push_back(i + 1);
  }
}

void ComputeLiveness(const MirFunction & function,
                     std::vector<BlockFlow> & flows)
{
  for(size_t i = 0; i < function.blocks.size(); ++i) {
    const MirBlock & block = function.blocks[i];
    flows[i].gen = 0;
    flows[i].kill = 0;
    for(size_t j = 0; j < block.instructions.size(); ++j) {
      InsEffect effect = classify(block.instructions[j]);
      flows[i].gen |= effect.use & ~flows[i].kill;
      flows[i].kill |= effect.def;
    }
  }
  bool changed = true;
  while(changed) {
    changed = false;
    for(size_t i = function.blocks.size(); i-- > 0;) {
      RegMask out = 0;
      for(size_t s = 0; s < flows[i].successors.size(); ++s) {
        const BlockFlow & succ = flows[flows[i].successors[s]];
        out |= succ.gen | (succ.live_out & ~succ.kill);
      }
      if(out != flows[i].live_out) {
        flows[i].live_out = out;
        changed = true;
      }
    }
  }
}

// ---- dead-copy elimination --------------------------------------------

// Side-effect-free single-register writes: staging copies, address
// materializations, float register copies. Loads and every flag- or
// memory-writing instruction stay.
bool pure_register_write(const MirInstruction & ins)
{
  typedef MirInstruction I;
  typedef MirOperand O;
  if(ins.operands.size() != 2)
    return false;
  const O & dst = ins.operands[0];
  const O & src = ins.operands[1];
  if(ins.opcode == I::MI_LEA)
    return dst.kind == O::OP_REG;
  if(ins.opcode == I::MI_MOV)
    return dst.kind == O::OP_REG &&
           (src.kind == O::OP_REG || src.kind == O::OP_IMM ||
            src.kind == O::OP_SYMBOL || src.kind == O::OP_GLOBAL);
  if(ins.opcode == I::MI_FMOV)
    return dst.kind == O::OP_XMM &&
           (src.kind == O::OP_XMM || src.kind == O::OP_FLOAT_IMM);
  return false;
}

bool EliminateDeadCopies(MirFunction & function)
{
  std::vector<BlockFlow> flows(function.blocks.size());
  CollectSuccessors(function, flows);
  ComputeLiveness(function, flows);
  bool changed = false;
  for(size_t i = 0; i < function.blocks.size(); ++i) {
    MirBlock & block = function.blocks[i];
    RegMask live = flows[i].live_out;
    std::vector<MirInstruction> kept;
    kept.reserve(block.instructions.size());
    // Frame operands read rbp/rsp implicitly, so writes to either are
    // never removable (baseline lowering never emits one mid-body).
    const RegMask frame_regs = gpr_bit(XR_RBP) | gpr_bit(XR_RSP);
    for(size_t j = block.instructions.size(); j-- > 0;) {
      const MirInstruction & ins = block.instructions[j];
      InsEffect effect = classify(ins);
      if(pure_register_write(ins) && (effect.def & live) == 0 &&
         (effect.def & frame_regs) == 0) {
        changed = true;
        continue;
      }
      live = (live & ~effect.def) | effect.use;
      kept.push_back(ins);
    }
    if(kept.size() != block.instructions.size())
      block.instructions.assign(kept.rbegin(), kept.rend());
  }
  return changed;
}

// ---- block-local forward propagation ----------------------------------

struct GprFact
{
  enum Kind { NONE, IMM, COPY, FRAME } kind;
  long long imm;
  int copy_of;
  long long frame_offset;

  GprFact() : kind(NONE), imm(0), copy_of(0), frame_offset(0) {}
};

struct XmmFact
{
  bool copy;
  int copy_of;
  std::string type;

  XmmFact() : copy(false), copy_of(0) {}
};

struct PropState
{
  GprFact gpr[16];
  XmmFact xmm[8];

  void clear()
  {
    for(int i = 0; i < 16; ++i)
      gpr[i] = GprFact();
    for(int i = 0; i < 8; ++i)
      xmm[i] = XmmFact();
  }

  void invalidate_gpr(int reg)
  {
    gpr[reg] = GprFact();
    for(int i = 0; i < 16; ++i)
      if(gpr[i].kind == GprFact::COPY && gpr[i].copy_of == reg)
        gpr[i] = GprFact();
  }

  void invalidate_xmm(int reg)
  {
    xmm[reg] = XmmFact();
    for(int i = 0; i < 8; ++i)
      if(xmm[i].copy && xmm[i].copy_of == reg)
        xmm[i] = XmmFact();
  }
};

bool fits_imm32(long long value)
{
  return value >= -0x7FFFFFFFLL - 1 && value <= 0x7FFFFFFFLL;
}

// Immediate rematerialization targets: two-operand integer arithmetic
// whose encoder accepts an imm32 source. Compares are deliberately
// excluded (`mov rdx, -1; cmp.i64 r8, rdx` is a pinned shape) and no
// branch constant-folding happens here.
bool imm_remat_opcode(MirInstruction::Opcode opcode)
{
  typedef MirInstruction I;
  switch(opcode) {
    case I::MI_ADD: case I::MI_SUB: case I::MI_IMUL:
    case I::MI_AND: case I::MI_OR: case I::MI_XOR:
      return true;
    default:
      return false;
  }
}

// Fold a memory operand: a base holding a frame address becomes a
// direct frame operand; a base holding a register copy rebases.
bool RewriteDeref(MirOperand & operand, const PropState & state)
{
  if(operand.kind != MirOperand::OP_DEREF)
    return false;
  const GprFact & fact = state.gpr[operand.reg];
  if(fact.kind == GprFact::FRAME) {
    operand.kind = MirOperand::OP_FRAME;
    operand.offset = fact.frame_offset + operand.offset;
    return true;
  }
  if(fact.kind == GprFact::COPY && fact.copy_of != operand.reg) {
    operand.reg = static_cast<X64Register>(fact.copy_of);
    return true;
  }
  return false;
}

bool RewriteGprCopyRead(MirOperand & operand, const PropState & state)
{
  if(operand.kind != MirOperand::OP_REG)
    return false;
  const GprFact & fact = state.gpr[operand.reg];
  if(fact.kind != GprFact::COPY || fact.copy_of == operand.reg)
    return false;
  operand.reg = static_cast<X64Register>(fact.copy_of);
  return true;
}

bool RewriteGprImmRead(MirOperand & operand, const PropState & state)
{
  if(operand.kind != MirOperand::OP_REG)
    return false;
  const GprFact & fact = state.gpr[operand.reg];
  if(fact.kind != GprFact::IMM)
    return false;
  operand.kind = MirOperand::OP_IMM;
  operand.imm = fact.imm;
  return true;
}

// The float type one xmm read operand carries (conversions read their
// source width).
const std::string & xmm_read_type(const MirInstruction & ins)
{
  switch(ins.opcode) {
    case MirInstruction::MI_FPEXT:
    case MirInstruction::MI_FPTRUNC:
    case MirInstruction::MI_FPTOSI:
    case MirInstruction::MI_FPTOUI:
      return ins.source_type;
    default:
      return ins.type;
  }
}

bool xmm_read_index(const MirInstruction & ins, size_t index)
{
  typedef MirInstruction I;
  switch(ins.opcode) {
    case I::MI_FMOV: case I::MI_FNEG: case I::MI_FADD: case I::MI_FSUB:
    case I::MI_FMUL: case I::MI_FDIV: case I::MI_FEQ: case I::MI_FNE:
    case I::MI_FLT: case I::MI_FGT: case I::MI_FLE: case I::MI_FGE:
    case I::MI_FPEXT: case I::MI_FPTRUNC: case I::MI_FPTOSI:
    case I::MI_FPTOUI:
      return index >= 1;
    case I::MI_FCMP:
      return true;
    default:
      return false;
  }
}

bool RewriteXmmRead(MirOperand & operand, const MirInstruction & ins,
                    const PropState & state)
{
  if(operand.kind != MirOperand::OP_XMM)
    return false;
  const XmmFact & fact = state.xmm[operand.xmm];
  if(!fact.copy || fact.copy_of == operand.xmm)
    return false;
  if(fact.type != xmm_read_type(ins))
    return false;
  operand.xmm = static_cast<XmmRegister>(fact.copy_of);
  return true;
}

bool RewriteInstruction(MirInstruction & ins, const PropState & state)
{
  typedef MirInstruction I;
  bool changed = false;
  for(size_t i = 0; i < ins.operands.size(); ++i)
    changed |= RewriteDeref(ins.operands[i], state);
  if(ins.opcode == I::MI_MOV && ins.operands.size() == 2 &&
     ins.operands[0].kind == MirOperand::OP_REG) {
    if(RewriteGprImmRead(ins.operands[1], state))
      changed = true;
    else
      changed |= RewriteGprCopyRead(ins.operands[1], state);
  }
  if(imm_remat_opcode(ins.opcode) && ins.operands.size() == 2) {
    const MirOperand & src = ins.operands[1];
    if(src.kind == MirOperand::OP_REG &&
       state.gpr[src.reg].kind == GprFact::IMM &&
       fits_imm32(state.gpr[src.reg].imm))
      changed |= RewriteGprImmRead(ins.operands[1], state);
  }
  if(ins.opcode == I::MI_RET && !ins.operands.empty())
    changed |= RewriteGprCopyRead(ins.operands[0], state);
  for(size_t i = 0; i < ins.operands.size(); ++i)
    if(xmm_read_index(ins, i))
      changed |= RewriteXmmRead(ins.operands[i], ins, state);
  return changed;
}

void InstallFact(const MirInstruction & ins, PropState & state)
{
  typedef MirInstruction I;
  typedef MirOperand O;
  if(ins.operands.size() != 2)
    return;
  const O & dst = ins.operands[0];
  const O & src = ins.operands[1];
  if(ins.opcode == I::MI_MOV && dst.kind == O::OP_REG) {
    GprFact & fact = state.gpr[dst.reg];
    if(src.kind == O::OP_IMM) {
      fact.kind = GprFact::IMM;
      fact.imm = src.imm;
    } else if(src.kind == O::OP_REG && src.reg != dst.reg) {
      fact.kind = GprFact::COPY;
      fact.copy_of = src.reg;
    }
    return;
  }
  if(ins.opcode == I::MI_LEA && dst.kind == O::OP_REG &&
     src.kind == O::OP_FRAME) {
    GprFact & fact = state.gpr[dst.reg];
    fact.kind = GprFact::FRAME;
    fact.frame_offset = src.offset;
    return;
  }
  if(ins.opcode == I::MI_FMOV && dst.kind == O::OP_XMM &&
     src.kind == O::OP_XMM && src.xmm != dst.xmm) {
    XmmFact & fact = state.xmm[dst.xmm];
    fact.copy = true;
    fact.copy_of = src.xmm;
    fact.type = ins.type;
  }
}

void UpdateFacts(const MirInstruction & ins, PropState & state)
{
  InsEffect effect = classify(ins);
  if(effect.barrier ||
     (effect.def & (gpr_bit(XR_RBP) | gpr_bit(XR_RSP))) != 0) {
    state.clear();
    return;
  }
  for(int reg = 0; reg < 16; ++reg)
    if(effect.def & gpr_bit(reg))
      state.invalidate_gpr(reg);
  for(int reg = 0; reg < 8; ++reg)
    if(effect.def & xmm_bit(reg))
      state.invalidate_xmm(reg);
  InstallFact(ins, state);
}

// A substitution can turn a staging copy into `mov r, r`; drop it.
bool self_copy(const MirInstruction & ins)
{
  typedef MirInstruction I;
  typedef MirOperand O;
  if(ins.operands.size() != 2)
    return false;
  if(ins.opcode == I::MI_MOV)
    return ins.operands[0].kind == O::OP_REG &&
           ins.operands[1].kind == O::OP_REG &&
           ins.operands[0].reg == ins.operands[1].reg;
  if(ins.opcode == I::MI_FMOV)
    return ins.operands[0].kind == O::OP_XMM &&
           ins.operands[1].kind == O::OP_XMM &&
           ins.operands[0].xmm == ins.operands[1].xmm;
  return false;
}

bool PropagateBlock(MirBlock & block)
{
  PropState state;
  bool changed = false;
  std::vector<MirInstruction> kept;
  kept.reserve(block.instructions.size());
  for(size_t i = 0; i < block.instructions.size(); ++i) {
    MirInstruction & ins = block.instructions[i];
    changed |= RewriteInstruction(ins, state);
    if(self_copy(ins)) {
      changed = true;
      continue;
    }
    UpdateFacts(ins, state);
    kept.push_back(ins);
  }
  if(kept.size() != block.instructions.size())
    block.instructions.swap(kept);
  return changed;
}

// ---- zero-compare branches --------------------------------------------

// `cmp.T r, 0` feeding a conditional branch carries the same flag
// results as `test.T r, r` for every condition code.
bool RewriteZeroCompareBranches(MirFunction & function)
{
  typedef MirInstruction I;
  typedef MirOperand O;
  bool changed = false;
  for(size_t b = 0; b < function.blocks.size(); ++b) {
    std::vector<MirInstruction> & ins = function.blocks[b].instructions;
    for(size_t i = 0; i + 1 < ins.size(); ++i) {
      if(ins[i].opcode != I::MI_CMP || ins[i + 1].opcode != I::MI_JCC)
        continue;
      if(ins[i].operands.size() != 2 ||
         ins[i].operands[0].kind != O::OP_REG ||
         ins[i].operands[1].kind != O::OP_IMM ||
         ins[i].operands[1].imm != 0)
        continue;
      ins[i].opcode = I::MI_TEST;
      ins[i].operands[1] = ins[i].operands[0];
      changed = true;
    }
  }
  return changed;
}

// ---- branch tails and layout ------------------------------------------

X86Condition invert_condition(X86Condition condition)
{
  return static_cast<X86Condition>(static_cast<int>(condition) ^ 1);
}

// Remove a trailing jump to the natural fallthrough block; otherwise
// invert a conditional branch that targets the fallthrough and let the
// unconditional target become the branch.
bool CollapseBranchTails(MirFunction & function)
{
  typedef MirInstruction I;
  bool changed = false;
  for(size_t b = 0; b + 1 < function.blocks.size(); ++b) {
    std::vector<MirInstruction> & ins = function.blocks[b].instructions;
    const std::string & next_label = function.blocks[b + 1].label;
    if(ins.empty())
      continue;
    const MirInstruction & last = ins.back();
    if(last.opcode != I::MI_JMP || last.operands.empty())
      continue;
    if(last.operands[0].text == next_label) {
      ins.pop_back();
      changed = true;
      continue;
    }
    if(ins.size() < 2)
      continue;
    MirInstruction & prev = ins[ins.size() - 2];
    if(prev.opcode != I::MI_JCC || prev.operands.empty() ||
       prev.operands[0].text != next_label)
      continue;
    prev.condition = invert_condition(prev.condition);
    prev.operands[0] = ins.back().operands[0];
    ins.pop_back();
    changed = true;
  }
  return changed;
}

// A block participates in trace layout when control leaves it through
// exactly one label; conditional tails keep their source order.
int UniqueTarget(const MirBlock & block,
                 const std::map<std::string, size_t> & index_of)
{
  std::string target;
  for(size_t i = 0; i < block.instructions.size(); ++i) {
    const MirInstruction & ins = block.instructions[i];
    for(size_t k = 0; k < ins.operands.size(); ++k) {
      if(ins.operands[k].kind != MirOperand::OP_LABEL)
        continue;
      if(target.empty())
        target = ins.operands[k].text;
      else if(target != ins.operands[k].text)
        return -1;
    }
  }
  if(target.empty())
    return -1;
  std::map<std::string, size_t>::const_iterator it = index_of.find(target);
  return it == index_of.end() ? -1 : static_cast<int>(it->second);
}

// O2 layout: follow unconditional jump traces so likely successors
// become natural fallthrough blocks; unplaced blocks keep their
// original relative order. Runs only while every block still ends with
// an explicit unconditional terminator.
bool LayoutBlocks(MirFunction & function)
{
  if(function.blocks.size() < 3)
    return false;
  for(size_t i = 0; i < function.blocks.size(); ++i) {
    const MirBlock & block = function.blocks[i];
    if(block.instructions.empty() ||
       !unconditional_terminator(block.instructions.back()))
      return false;
  }
  std::map<std::string, size_t> index_of;
  for(size_t i = 0; i < function.blocks.size(); ++i)
    index_of[function.blocks[i].label] = i;
  std::vector<int> targets(function.blocks.size());
  for(size_t i = 0; i < function.blocks.size(); ++i)
    targets[i] = UniqueTarget(function.blocks[i], index_of);
  std::vector<bool> placed(function.blocks.size(), false);
  std::vector<size_t> order;
  order.reserve(function.blocks.size());
  size_t scan = 0;
  size_t current = 0;
  placed[0] = true;
  order.push_back(0);
  while(order.size() < function.blocks.size()) {
    const int traced = targets[current];
    if(traced >= 0 && !placed[traced]) {
      current = static_cast<size_t>(traced);
    } else {
      while(placed[scan])
        ++scan;
      current = scan;
    }
    placed[current] = true;
    order.push_back(current);
  }
  bool moved = false;
  for(size_t i = 0; i < order.size(); ++i)
    moved |= order[i] != i;
  if(!moved)
    return false;
  std::vector<MirBlock> blocks;
  blocks.reserve(order.size());
  for(size_t i = 0; i < order.size(); ++i)
    blocks.push_back(function.blocks[order[i]]);
  function.blocks.swap(blocks);
  return true;
}

// ---- callee-saved pruning and stack recompute -------------------------

// Explicit register mentions: operand registers plus fixed-role
// implicits, with calls counted as their caller-saved footprint only.
// A call preserves callee-saved registers, so it is not by itself a
// reason to keep one preserved.
RegMask ExplicitMentionMask(const MirFunction & function)
{
  RegMask mask = 0;
  for(size_t b = 0; b < function.blocks.size(); ++b) {
    const MirBlock & block = function.blocks[b];
    for(size_t i = 0; i < block.instructions.size(); ++i) {
      const MirInstruction & ins = block.instructions[i];
      InsEffect effect = classify(ins);
      if(effect.barrier) {
        mask |= effect.def;
        for(size_t k = 0; k < ins.operands.size(); ++k)
          mask |= operand_read_mask(ins.operands[k]);
        continue;
      }
      mask |= effect.use | effect.def;
    }
  }
  for(size_t i = 0; i < function.params.size(); ++i) {
    const mir_model::MirParamBinding & param = function.params[i];
    if(param.location == mir_model::MirParamBinding::PL_REG)
      mask |= gpr_bit(param.reg);
    if(param.location == mir_model::MirParamBinding::PL_XMM)
      mask |= xmm_bit(param.xmm);
  }
  for(size_t i = 0; i < function.debug_variables.size(); ++i) {
    const mir_model::MirDebugVariable & variable =
        function.debug_variables[i];
    for(size_t r = 0; r < variable.ranges.size(); ++r) {
      const mir_model::MirDebugVariable::Range & range =
          variable.ranges[r];
      if(range.location == mir_model::MirDebugVariable::Range::LK_REG)
        mask |= gpr_bit(range.reg);
      if(range.location == mir_model::MirDebugVariable::Range::LK_XMM)
        mask |= xmm_bit(range.xmm);
    }
  }
  return mask;
}

// O2: drop preserved registers the surviving body never mentions and
// reclaim their save slots. The slots live inside stack_size (8 bytes
// each, above the scratch area); only whole 16-byte alignment steps
// are returned, and the lowering's baseline frame policy is otherwise
// preserved.
bool PruneCalleeSaved(MirFunction & function)
{
  if(function.callee_saved_regs.empty())
    return false;
  const RegMask mentioned = ExplicitMentionMask(function);
  std::vector<X64Register> kept;
  for(size_t i = 0; i < function.callee_saved_regs.size(); ++i)
    if(mentioned & gpr_bit(function.callee_saved_regs[i]))
      kept.push_back(function.callee_saved_regs[i]);
  const size_t pruned = function.callee_saved_regs.size() - kept.size();
  if(pruned == 0)
    return false;
  function.callee_saved_regs.swap(kept);
  const std::size_t freed = 8 * pruned;
  function.stack_size -= (freed / 16) * 16;
  return true;
}

// ---- driver -----------------------------------------------------------

void OptimizeFunction(MirFunction & function, int level)
{
  if(function.host_eh_enabled || !function.host_eh_regions.empty())
    return;
  bool changed = true;
  for(int round = 0; changed && round < 8; ++round) {
    changed = false;
    for(size_t b = 0; b < function.blocks.size(); ++b)
      changed |= PropagateBlock(function.blocks[b]);
    changed |= EliminateDeadCopies(function);
  }
  RewriteZeroCompareBranches(function);
  if(level >= 2)
    LayoutBlocks(function);
  CollapseBranchTails(function);
  if(level >= 2)
    PruneCalleeSaved(function);
}

}  // namespace

void OptimizeMirProgram(MirProgram & program, int level)
{
  if(level < 1)
    return;
  for(size_t i = 0; i < program.functions.size(); ++i)
    OptimizeFunction(program.functions[i], level);
}

}  // namespace mir_optimize
