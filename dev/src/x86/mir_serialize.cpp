#include "mir_model.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

// MIR text serialization. The dump is the tested view of the backend
// program model: pa28 strict fixtures compare it byte-for-byte after
// host-target header normalization, so every spelling here is pinned
// by the checked-in refs.

namespace mir_model {

namespace {

const char * gpr_name(X64Register reg)
{
  switch(reg) {
    case XR_RAX: return "rax";
    case XR_RCX: return "rcx";
    case XR_RDX: return "rdx";
    case XR_RBX: return "rbx";
    case XR_RSP: return "rsp";
    case XR_RBP: return "rbp";
    case XR_RSI: return "rsi";
    case XR_RDI: return "rdi";
    case XR_R8: return "r8";
    case XR_R9: return "r9";
    case XR_R10: return "r10";
    case XR_R11: return "r11";
    case XR_R12: return "r12";
    case XR_R13: return "r13";
    case XR_R14: return "r14";
    case XR_R15: return "r15";
  }
  throw std::logic_error("unknown gpr in mir serialization");
}

const char * xmm_name(XmmRegister reg)
{
  switch(reg) {
    case XMM_0: return "xmm0";
    case XMM_1: return "xmm1";
    case XMM_2: return "xmm2";
    case XMM_3: return "xmm3";
    case XMM_4: return "xmm4";
    case XMM_5: return "xmm5";
    case XMM_6: return "xmm6";
    case XMM_7: return "xmm7";
  }
  throw std::logic_error("unknown xmm in mir serialization");
}

const char * condition_suffix(X86Condition condition)
{
  switch(condition) {
    case XC_E: return "e";
    case XC_NE: return "ne";
    case XC_B: return "b";
    case XC_AE: return "ae";
    case XC_BE: return "be";
    case XC_A: return "a";
    case XC_L: return "l";
    case XC_GE: return "ge";
    case XC_LE: return "le";
    case XC_G: return "g";
    case XC_S: return "s";
    case XC_NS: return "ns";
    case XC_P: return "p";
    case XC_NP: return "np";
    default: break;
  }
  throw std::logic_error("unknown condition in mir serialization");
}

std::string frame_ref(long long offset)
{
  std::ostringstream out;
  if(offset < 0)
    out << "[rbp-" << -offset << "]";
  else
    out << "[rbp+" << offset << "]";
  return out.str();
}

std::string operand_text(const Operand & operand)
{
  std::ostringstream out;
  switch(operand.kind) {
    case Operand::OP_REG:
      return gpr_name(operand.reg);
    case Operand::OP_XMM:
      return xmm_name(operand.xmm);
    case Operand::OP_IMM:
      out << operand.imm;
      return out.str();
    case Operand::OP_FLOAT_IMM:
      return operand.text;
    case Operand::OP_SYMBOL:
      return "@" + operand.text;
    case Operand::OP_GLOBAL:
      return "@" + operand.text;
    case Operand::OP_FRAME:
      return frame_ref(operand.offset);
    case Operand::OP_DEREF:
      if(operand.offset == 0)
        return std::string("[") + gpr_name(operand.reg) + "]";
      out << "[" << gpr_name(operand.reg)
          << (operand.offset > 0 ? "+" : "-")
          << (operand.offset > 0 ? operand.offset : -operand.offset)
          << "]";
      return out.str();
    case Operand::OP_LABEL:
      return "^" + operand.text;
  }
  throw std::logic_error("unknown operand in mir serialization");
}

// `mnemonic op, op, ...` with the operand list joined by ", ".
void write_operands(std::ostream & out,
                    const Instruction & instruction,
                    const std::string & mnemonic)
{
  out << mnemonic;
  for(size_t i = 0; i < instruction.operands.size(); ++i)
    out << (i == 0 ? " " : ", ") << operand_text(instruction.operands[i]);
  out << "\n";
}

std::string span_text(const Instruction & instruction)
{
  std::ostringstream out;
  out << instruction.byte_count << "x" << instruction.byte_alignment;
  return out.str();
}

void write_instruction(std::ostream & out, const Instruction & ins)
{
  const std::string & ty = ins.type;
  switch(ins.opcode) {
    case Instruction::MI_MOV: return write_operands(out, ins, "mov");
    case Instruction::MI_LEA: return write_operands(out, ins, "lea");
    case Instruction::MI_LOAD: return write_operands(out, ins, "load." + ty);
    case Instruction::MI_STORE: return write_operands(out, ins, "store." + ty);
    case Instruction::MI_MFENCE: return write_operands(out, ins, "mfence");
    case Instruction::MI_LOCK_XADD:
      return write_operands(out, ins, "lock_xadd." + ty);
    case Instruction::MI_XCHG: return write_operands(out, ins, "xchg." + ty);
    case Instruction::MI_LOCK_CMPXCHG:
      return write_operands(out, ins, "lock_cmpxchg." + ty);
    case Instruction::MI_ADD: return write_operands(out, ins, "add");
    case Instruction::MI_ADC: return write_operands(out, ins, "adc");
    case Instruction::MI_SUB: return write_operands(out, ins, "sub");
    case Instruction::MI_SBB: return write_operands(out, ins, "sbb");
    case Instruction::MI_IMUL: return write_operands(out, ins, "imul");
    case Instruction::MI_MUL: return write_operands(out, ins, "mul");
    case Instruction::MI_AND: return write_operands(out, ins, "and");
    case Instruction::MI_OR: return write_operands(out, ins, "or");
    case Instruction::MI_XOR: return write_operands(out, ins, "xor");
    case Instruction::MI_NEG: return write_operands(out, ins, "neg");
    case Instruction::MI_NOT: return write_operands(out, ins, "not");
    case Instruction::MI_BSWAP: return write_operands(out, ins, "bswap");
    case Instruction::MI_CMP: return write_operands(out, ins, "cmp." + ty);
    case Instruction::MI_TEST: return write_operands(out, ins, "test." + ty);
    case Instruction::MI_SETCC:
      return write_operands(out, ins,
                            std::string("set") + condition_suffix(ins.condition));
    case Instruction::MI_JCC:
      return write_operands(out, ins,
                            std::string("j") + condition_suffix(ins.condition));
    case Instruction::MI_JMP: return write_operands(out, ins, "jmp");
    case Instruction::MI_MOVZX: return write_operands(out, ins, "movzx");
    case Instruction::MI_SEXT: return write_operands(out, ins, "sext." + ty);
    case Instruction::MI_ZEXT: return write_operands(out, ins, "zext." + ty);
    case Instruction::MI_CQO: return write_operands(out, ins, "cqo");
    case Instruction::MI_IDIV: return write_operands(out, ins, "idiv");
    case Instruction::MI_DIV: return write_operands(out, ins, "div");
    case Instruction::MI_SHL_CL:
      out << "shl " << operand_text(ins.operands[0]) << ", cl\n";
      return;
    case Instruction::MI_SHR_CL:
      out << "shr " << operand_text(ins.operands[0]) << ", cl\n";
      return;
    case Instruction::MI_SAR_CL:
      out << "sar " << operand_text(ins.operands[0]) << ", cl\n";
      return;
    case Instruction::MI_TLS_ADDR:
      return write_operands(out, ins, "tls_addr");
    case Instruction::MI_CALL: return write_operands(out, ins, "call");
    case Instruction::MI_CALL_INDIRECT:
      out << "call *" << operand_text(ins.operands[0]) << "\n";
      return;
    case Instruction::MI_COPY_BYTES:
      out << "copy_bytes " << span_text(ins) << ", "
          << operand_text(ins.operands[0]) << ", "
          << operand_text(ins.operands[1]) << "\n";
      return;
    case Instruction::MI_ZERO_BYTES:
      out << "zero_bytes " << span_text(ins) << ", "
          << operand_text(ins.operands[0]) << "\n";
      return;
    case Instruction::MI_FMOV: return write_operands(out, ins, "fmov." + ty);
    case Instruction::MI_FNEG: return write_operands(out, ins, "fneg." + ty);
    case Instruction::MI_FADD: return write_operands(out, ins, "fadd." + ty);
    case Instruction::MI_FSUB: return write_operands(out, ins, "fsub." + ty);
    case Instruction::MI_FMUL: return write_operands(out, ins, "fmul." + ty);
    case Instruction::MI_FDIV: return write_operands(out, ins, "fdiv." + ty);
    case Instruction::MI_FEQ: return write_operands(out, ins, "feq." + ty);
    case Instruction::MI_FNE: return write_operands(out, ins, "fne." + ty);
    case Instruction::MI_FLT: return write_operands(out, ins, "flt." + ty);
    case Instruction::MI_FGT: return write_operands(out, ins, "fgt." + ty);
    case Instruction::MI_FLE: return write_operands(out, ins, "fle." + ty);
    case Instruction::MI_FGE: return write_operands(out, ins, "fge." + ty);
    case Instruction::MI_FCMP: return write_operands(out, ins, "fcmp." + ty);
    case Instruction::MI_FSTP: return write_operands(out, ins, "fstp." + ty);
    case Instruction::MI_SITOFP:
      return write_operands(out, ins,
                            "sitofp." + ins.source_type + "." + ty);
    case Instruction::MI_UITOFP:
      return write_operands(out, ins,
                            "uitofp." + ins.source_type + "." + ty);
    case Instruction::MI_FPTOSI:
      return write_operands(out, ins,
                            "fptosi." + ins.source_type + "." + ty);
    case Instruction::MI_FPTOUI:
      return write_operands(out, ins,
                            "fptoui." + ins.source_type + "." + ty);
    case Instruction::MI_FPEXT:
      return write_operands(out, ins,
                            "fpext." + ins.source_type + "." + ty);
    case Instruction::MI_FPTRUNC:
      return write_operands(out, ins,
                            "fptrunc." + ins.source_type + "." + ty);
    case Instruction::MI_FRET: return write_operands(out, ins, "fret." + ty);
    case Instruction::MI_RET: return write_operands(out, ins, "ret");
    case Instruction::MI_EXIT: return write_operands(out, ins, "exit");
    default: break;
  }
  throw std::logic_error("unknown instruction in mir serialization");
}

void write_global(std::ostream & out, const GlobalDefinition & global)
{
  out << "global @" << global.name;
  if(global.readonly)
    out << " readonly";
  if(global.thread_local_storage)
    out << " thread_local";
  out << "\n";
  if(global.storage_kind == GlobalDefinition::GS_SCALAR) {
    out << "  storage scalar " << global.type << "\n";
    switch(global.init_kind) {
      case GlobalDefinition::GI_ZERO:
        out << "  init " << global.type << " 0\n";
        break;
      case GlobalDefinition::GI_INTEGER:
        out << "  init " << global.type << " " << global.int_value << "\n";
        break;
      case GlobalDefinition::GI_FLOAT:
        out << "  init " << global.type << " " << global.literal_text << "\n";
        break;
      case GlobalDefinition::GI_ADDR:
        out << "  init addr @" << global.symbol;
        if(global.addr_addend > 0)
          out << " + " << global.addr_addend;
        else if(global.addr_addend < 0)
          out << " - " << -global.addr_addend;
        out << "\n";
        break;
    }
    return;
  }
  out << "  storage data\n";
  for(size_t i = 0; i < global.data_items.size(); ++i) {
    const GlobalDefinition::DataItem & item = global.data_items[i];
    switch(item.kind) {
      case GlobalDefinition::DataItem::ITEM_INTEGER:
        out << "  item " << item.type << " " << item.int_value << "\n";
        break;
      case GlobalDefinition::DataItem::ITEM_FLOAT:
        out << "  item " << item.type << " " << item.literal_text << "\n";
        break;
      case GlobalDefinition::DataItem::ITEM_ADDR:
        out << "  item ptr addr @" << item.symbol;
        if(item.addr_addend > 0)
          out << " + " << item.addr_addend;
        else if(item.addr_addend < 0)
          out << " - " << -item.addr_addend;
        out << "\n";
        break;
      case GlobalDefinition::DataItem::ITEM_ZERO:
        out << "  item zero " << item.zero_bytes << "\n";
        break;
    }
  }
}

void write_param(std::ostream & out, const ParamBinding & param)
{
  out << "    param %" << param.name << " -> ";
  switch(param.location) {
    case ParamBinding::PL_REG:
      out << gpr_name(param.reg);
      break;
    case ParamBinding::PL_XMM:
      out << xmm_name(param.xmm);
      break;
    case ParamBinding::PL_STACK:
      out << frame_ref(param.stack_offset);
      break;
  }
  out << " : " << param.type << "\n";
}

const char * frame_binding_kind(const FrameBinding & binding)
{
  switch(binding.kind) {
    case FrameBinding::FB_PARAM_SLOT: return "param-slot";
    case FrameBinding::FB_SLOT: return "slot";
    case FrameBinding::FB_TEMP: return "temp";
  }
  throw std::logic_error("unknown frame binding in mir serialization");
}

const char * frame_binding_sigil(const FrameBinding & binding)
{
  return binding.kind == FrameBinding::FB_SLOT ? "$" : "%";
}

void write_function(std::ostream & out, const Function & function)
{
  out << "function @" << function.name << "\n";
  out << "  abi\n";
  for(size_t i = 0; i < function.params.size(); ++i)
    write_param(out, function.params[i]);
  out << "    return " << function.return_type << " -> ";
  if(function.return_type == "void")
    out << "void";
  else if(function.return_type == "f32" || function.return_type == "f64")
    out << "xmm0";
  else if(function.return_type == "f80")
    out << "st0";
  else
    out << "rax";
  out << "\n";
  out << "  frame\n";
  out << "    stack_size " << function.stack_size << "\n";
  out << "    scratch_bytes " << function.scratch_bytes << "\n";
  if(!function.callee_saved_regs.empty()) {
    out << "    preserve";
    for(size_t i = 0; i < function.callee_saved_regs.size(); ++i)
      out << " " << gpr_name(function.callee_saved_regs[i]);
    out << "\n";
  }
  for(size_t i = 0; i < function.frame_bindings.size(); ++i) {
    const FrameBinding & binding = function.frame_bindings[i];
    out << "    " << frame_binding_kind(binding) << " "
        << frame_binding_sigil(binding) << binding.name << " -> "
        << frame_ref(binding.offset) << " : " << binding.type << "\n";
  }
  for(size_t i = 0; i < function.blocks.size(); ++i) {
    const Block & block = function.blocks[i];
    out << "\n  block ^" << block.label << "\n";
    for(size_t j = 0; j < block.instructions.size(); ++j) {
      out << "    ";
      write_instruction(out, block.instructions[j]);
    }
  }
}

}  // namespace

std::string serialize_mir_program(const MirProgram & program)
{
  std::ostringstream out;
  out << "machine_ir x86_64 " << program.target << "\n";
  if(!program.startup.empty()) {
    out << "\nstartup\n";
    for(size_t i = 0; i < program.startup.size(); ++i) {
      out << "    ";
      write_instruction(out, program.startup[i]);
    }
  }
  for(size_t i = 0; i < program.globals.size(); ++i) {
    out << "\n";
    write_global(out, program.globals[i]);
  }
  for(size_t i = 0; i < program.functions.size(); ++i) {
    out << "\n";
    write_function(out, program.functions[i]);
  }
  return out.str();
}

void write_mir_program_file(const std::string & path,
                            const MirProgram & program)
{
  std::ofstream out(path.c_str(), std::ios::out | std::ios::binary);
  if(!out)
    throw std::runtime_error("unable to write machine IR file: " + path);
  out << serialize_mir_program(program);
  out.close();
  if(!out.good())
    throw std::runtime_error("unable to write machine IR file: " + path);
}

}  // namespace mir_model
