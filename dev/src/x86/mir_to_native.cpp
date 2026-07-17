#include "x86/mir_to_native.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "x86/elf_program.h"
#include "x86/mir_native_data.h"

using std::logic_error;
using std::runtime_error;

// Machine-IR native emission: program walk, global data encoding,
// the literal pool, and instruction encoding through the PA9 x86-64
// encoder.
//
// Each function becomes one image item. Blocks are encoded in order
// into the item; jcc/jmp rel32 fields that target blocks are recorded
// as local fixups and back-patched once every block offset is known
// (all encodings are length-stable, so offsets are final). The
// encoder emits placeholder PCREL patches for those forms; Finish
// drops label-less PCREL patches so image layout cannot overwrite the
// locally resolved fields.
//
// f32/f64 values live in xmm registers with xmm15 as the universal
// staging register; f80 values live in 16-byte memory slots bridged
// through the x87 stack and the three 16-byte frame scratch slots.

namespace {

typedef mir_model::MirInstruction Ins;
typedef mir_model::MirOperand Op;
typedef mir_model::MirGlobalDefinition Global;

const long double kTwoTo63 = 9223372036854775808.0L;
const long double kTwoTo64 = 18446744073709551616.0L;

// MIR width/data helpers are shared with the global-data encoder.
using mir_native::AppendFloatBits;
using mir_native::EncodeGlobal;
using mir_native::EncodeTlsWrapperItem;
using mir_native::AppendLittleEndian;
using mir_native::ParseFloatLiteral;
using mir_native::ScalarSize;
using mir_native::TypeBits;

int Reg(const Op & op)
{
	return static_cast<int>(op.reg);
}

int Xmm(const Op & op)
{
	return static_cast<int>(op.xmm);
}

bool IsMemOperand(const Op & op)
{
	return op.kind == Op::OP_FRAME || op.kind == Op::OP_DEREF ||
	       op.kind == Op::OP_GLOBAL;
}

bool FitsImm32(long long value)
{
	return value >= -2147483648LL && value <= 2147483647LL;
}

unsigned long long CheckedImm32(long long value)
{
	if (!FitsImm32(value))
		throw logic_error("machine IR immediate exceeds imm32");
	return static_cast<unsigned long long>(value);
}

long long SignExtendValue(long long value, int bits)
{
	if (bits >= 64)
		return value;
	int shift = 64 - bits;
	return static_cast<long long>(
		static_cast<unsigned long long>(value) << shift) >> shift;
}

unsigned long long ZeroExtendValue(long long value, int bits)
{
	unsigned long long wide = static_cast<unsigned long long>(value);
	if (bits >= 64)
		return wide;
	return wide & ((1ULL << bits) - 1);
}

EX86Mnemonic MovLoad(int bits)
{
	return bits == 32 ? X86_MOVSS_RM : X86_MOVSD_RM;
}

EX86Mnemonic MovStore(int bits)
{
	return bits == 32 ? X86_MOVSS_MR : X86_MOVSD_MR;
}

EX86Mnemonic UcomisFor(int bits)
{
	return bits == 32 ? X86_UCOMISS : X86_UCOMISD;
}

EX86Mnemonic CvttFor(int bits)
{
	return bits == 32 ? X86_CVTTSS2SI : X86_CVTTSD2SI;
}

EX86Mnemonic SseArith(Ins::Opcode op, int bits)
{
	switch (op)
	{
	case Ins::MI_FADD: return bits == 32 ? X86_ADDSS : X86_ADDSD;
	case Ins::MI_FSUB: return bits == 32 ? X86_SUBSS : X86_SUBSD;
	case Ins::MI_FMUL: return bits == 32 ? X86_MULSS : X86_MULSD;
	default: return bits == 32 ? X86_DIVSS : X86_DIVSD;
	}
}

EX86Mnemonic X87Arith(Ins::Opcode op)
{
	switch (op)
	{
	case Ins::MI_FADD: return X86_FADDP;
	case Ins::MI_FSUB: return X86_FSUBP;
	case Ins::MI_FMUL: return X86_FMULP;
	default: return X86_FDIVP;
	}
}

// One pooled data constant (float literals, sign masks).
struct PoolEntry
{
	std::vector<unsigned char> bytes;
	std::size_t align;
	int label;
};

// Program-wide names and the literal pool. Labels are dense image
// label ids: functions and globals first, pool constants (and, for
// relocatable modules, referenced external symbols) as they are
// discovered during function encoding.
class ProgramEnv : public mir_native::ISymbolLabels
{
public:
	ProgramEnv(const mir_model::MirProgram & program, bool allow_external);

	const mir_model::MirProgram & program() const { return program_; }
	int SymbolLabel(const std::string & name);  // ISymbolLabels
	bool HasFunction(const std::string & name) const;
	const std::string & TlsBackingGlobal(const std::string & wrapper) const;

	// Label of a pooled constant holding `value` in the given float
	// format (32/64/80 bits), deduplicated by representation.
	int FloatConstantLabel(int bits, long double value);
	int ByteConstantLabel(const std::string & key,
	                      const unsigned char * data, std::size_t size,
	                      std::size_t align);

	int label_count() const { return next_label_; }
	const std::vector<PoolEntry> & pool() const { return pool_; }
	const std::vector<std::string> & label_names() const
	{
		return label_names_;
	}

private:
	const mir_model::MirProgram & program_;
	bool allow_external_;
	std::map<std::string, int> symbol_labels_;
	std::set<std::string> function_names_;
	std::map<std::string, int> pool_labels_;
	std::vector<PoolEntry> pool_;
	std::vector<std::string> label_names_;  // label id -> name ("" pool)
	int next_label_;
};

ProgramEnv::ProgramEnv(const mir_model::MirProgram & program,
                       bool allow_external)
	: program_(program), allow_external_(allow_external), next_label_(0)
{
	for (std::size_t i = 0; i < program.functions.size(); i++)
	{
		symbol_labels_[program.functions[i].name] = next_label_++;
		label_names_.push_back(program.functions[i].name);
		function_names_.insert(program.functions[i].name);
	}
	for (std::size_t i = 0; i < program.globals.size(); i++)
	{
		symbol_labels_[program.globals[i].name] = next_label_++;
		label_names_.push_back(program.globals[i].name);
	}
}

int ProgramEnv::SymbolLabel(const std::string & name)
{
	std::map<std::string, int>::const_iterator it =
		symbol_labels_.find(name);
	if (it != symbol_labels_.end())
		return it->second;
	if (!allow_external_)
		throw runtime_error("machine IR references unknown symbol: " +
		                    name);
	// Relocatable-module encoding: an unknown name is an external
	// reference the linker resolves; it claims a label with no item.
	int label = next_label_++;
	symbol_labels_[name] = label;
	label_names_.push_back(name);
	return label;
}

bool ProgramEnv::HasFunction(const std::string & name) const
{
	return function_names_.count(name) != 0;
}

const std::string & ProgramEnv::TlsBackingGlobal(
	const std::string & wrapper) const
{
	std::map<std::string, std::string>::const_iterator it =
		program_.tls_wrappers.find(wrapper);
	if (it == program_.tls_wrappers.end())
		throw runtime_error("machine IR references unknown TLS wrapper: " +
		                    wrapper);
	return it->second;
}

int ProgramEnv::FloatConstantLabel(int bits, long double value)
{
	std::vector<unsigned char> bytes;
	AppendFloatBits(bytes,
	                bits == 32 ? "f32" : bits == 64 ? "f64" : "f80",
	                value);
	std::ostringstream key;
	key << "f" << bits << ":";
	for (std::size_t i = 0; i < bytes.size(); i++)
		key << std::hex << static_cast<int>(bytes[i]) << ",";
	return ByteConstantLabel(key.str(), bytes.data(), bytes.size(),
	                         bits == 32 ? 4 : bits == 64 ? 8 : 16);
}

int ProgramEnv::ByteConstantLabel(const std::string & key,
                                  const unsigned char * data,
                                  std::size_t size, std::size_t align)
{
	std::map<std::string, int>::const_iterator it = pool_labels_.find(key);
	if (it != pool_labels_.end())
		return it->second;
	PoolEntry entry;
	entry.bytes.assign(data, data + size);
	entry.align = align;
	entry.label = next_label_++;
	label_names_.push_back("");
	pool_.push_back(entry);
	pool_labels_[key] = entry.label;
	return entry.label;
}

// Encodes one function (or the startup sled) into an image item.
class FunctionEncoder
{
public:
	FunctionEncoder(ProgramEnv & env, const mir_model::MirFunction * fn);

	void EncodeFunction(ImageItem & item);
	void EncodeStartup(ImageItem & item);
	void FinishEhFacts(NativeFunctionEh & eh) const;

private:
	struct BlockFixup
	{
		std::size_t offset;  // rel32 field start within the item bytes
		std::string target;
	};

	void EmitPrologue();
	void EmitEpilogue();
	void EmitInstruction(const Ins & ins);
	void EmitMov(const Ins & ins);
	void EmitLoad(const Ins & ins);
	void EmitExtend(const Ins & ins);
	void EmitBinary(const Ins & ins);
	void EmitBswap(const Ins & ins);
	void EmitCompare(const Ins & ins, bool is_test);
	void EmitCompareImmLhs(const Ins & ins, int width, bool is_test);
	void EmitBranch(const Ins & ins);
	void EmitTlsAddr(const Ins & ins);
	void EmitEhLanding();
	void Finish(ImageItem & item);
	X86Mem SavedRegSlot(std::size_t index) const;
	X86Mem MemOperand(const Op & op, long long extra);
	int ScratchGpr(const Ins & ins) const;

	void EmitFloatInstruction(const Ins & ins);
	void FldFloatViaScratch(int bits, const Op & op, int slot);
	void EmitFmov(const Ins & ins);
	void EmitFneg(const Ins & ins);
	void EmitFarith(const Ins & ins);
	void EmitFbool(const Ins & ins);
	void EmitSitofp(const Ins & ins);
	void EmitUitofp(const Ins & ins);
	void EmitFptosi(const Ins & ins);
	void EmitFptoui(const Ins & ins);
	void EmitFpConvert(const Ins & ins);
	X86Mem FloatRm(int bits, const Op & op);
	X86Mem PoolFloatMem(int bits, long double value);
	X86Mem ScratchSlot(int slot, long long extra);
	void CopyToXmm15(int bits, const Op & op);
	void StoreXmmTo(int bits, const Op & dst, int xmm);
	void FldOperand(int bits, const Op & op);
	void FldFloatSource(int bits, const Op & op);
	void UcomisFlags(int bits, const Op & a, const Op & b);
	void EmitFloatConstantMove(int bits, const Op & dst,
	                           long double value);
	void EmitUnsigned64ToFloat(int dbits, const Op & dst, int src_reg);
	void SetTruncationCw(int temp_gpr);
	void RestoreCw();
	void EmitTruncatingFistp(int dst_reg);

	ProgramEnv & env_;
	const mir_model::MirFunction * fn_;
	X86CodeBuffer code_;
	std::map<std::string, std::size_t> block_offsets_;
	std::vector<BlockFixup> fixups_;
	// host-EH facts recorded while encoding (see FinishEhFacts)
	std::vector<NativeEhCallSite> eh_call_sites_;
	std::size_t prologue_push_end_ = 0;
	std::size_t prologue_frame_end_ = 0;
	std::size_t prologue_end_ = 0;
};

FunctionEncoder::FunctionEncoder(ProgramEnv & env,
                                 const mir_model::MirFunction * fn)
	: env_(env), fn_(fn)
{
}

void FunctionEncoder::EncodeFunction(ImageItem & item)
{
	EmitPrologue();
	for (std::size_t b = 0; b < fn_->blocks.size(); b++)
	{
		const mir_model::MirBlock & block = fn_->blocks[b];
		block_offsets_[block.label] = code_.bytes().size();
		for (std::size_t i = 0; i < block.instructions.size(); i++)
			EmitInstruction(block.instructions[i]);
	}
	Finish(item);
}

void FunctionEncoder::EncodeStartup(ImageItem & item)
{
	const std::vector<Ins> & startup = env_.program().startup;
	if (startup.empty())
	{
		code_.MovRegImm(X86_RDI, X86Imm::Constant(0));
		code_.MovRegImm(X86_RAX, X86Imm::Constant(60));
		code_.Syscall();
	}
	for (std::size_t i = 0; i < startup.size(); i++)
		EmitInstruction(startup[i]);
	Finish(item);
}

void FunctionEncoder::Finish(ImageItem & item)
{
	item.is_code = true;
	item.bytes = code_.bytes();
	std::vector<X86Patch> patches = code_.patches();
	for (std::size_t i = 0; i < patches.size(); i++)
	{
		// Label-less fields are fully resolved while encoding (the
		// intra-function jumps here, immediate constants at emission);
		// only symbol-relative patches reach the layout pass.
		if (!patches[i].imm.has_label)
			continue;
		item.patches.push_back(patches[i]);
	}
	for (std::size_t i = 0; i < fixups_.size(); i++)
	{
		std::map<std::string, std::size_t>::const_iterator it =
			block_offsets_.find(fixups_[i].target);
		if (it == block_offsets_.end())
			throw runtime_error("machine IR jump to unknown block: " +
			                    fixups_[i].target);
		long long rel = static_cast<long long>(it->second) -
		                static_cast<long long>(fixups_[i].offset + 4);
		for (int b = 0; b < 4; b++)
			item.bytes[fixups_[i].offset + static_cast<std::size_t>(b)] =
				static_cast<unsigned char>(rel >> (8 * b));
	}
}

// The typed host-EH and frame facts of the just-encoded function:
// resolved landing-pad offsets, recorded call-site ranges, and the
// prologue/callee-save shape CFI generation consumes.
void FunctionEncoder::FinishEhFacts(NativeFunctionEh & eh) const
{
	eh.name = fn_->name;
	eh.code_size = code_.bytes().size();
	eh.prologue_push_end = prologue_push_end_;
	eh.prologue_frame_end = prologue_frame_end_;
	eh.prologue_end = prologue_end_;
	eh.stack_size = fn_->stack_size;
	for (std::size_t i = 0; i < fn_->callee_saved_regs.size(); i++)
	{
		X86Mem slot = SavedRegSlot(i);
		eh.saved_regs.push_back(std::make_pair(
			fn_->callee_saved_regs[i],
			static_cast<long long>(slot.disp)));
	}
	for (std::size_t r = 0; r < fn_->host_eh_regions.size(); r++)
	{
		const mir_model::HostEhRegion & plan = fn_->host_eh_regions[r];
		std::map<std::string, std::size_t>::const_iterator it =
			block_offsets_.find(plan.landing_label);
		if (it == block_offsets_.end())
			throw runtime_error("host-EH landing pad has no block: " +
			                    plan.landing_label);
		NativeEhRegion region;
		region.landing_offset = it->second;
		region.cleanup = plan.cleanup;
		region.parent = plan.parent;
		region.clauses = plan.clauses;
		eh.regions.push_back(region);
	}
	eh.call_sites = eh_call_sites_;
}

X86Mem FunctionEncoder::SavedRegSlot(std::size_t index) const
{
	long long offset =
		static_cast<long long>(fn_->stack_size) -
		static_cast<long long>(fn_->scratch_bytes) -
		8 * static_cast<long long>(index);
	return X86Mem(X86_RBP, static_cast<int>(-offset));
}

void FunctionEncoder::EmitPrologue()
{
	if (fn_->zero_frame_pointer)
		code_.Alu(X86_XOR, 32, X86_RBP, X86_RBP);
	code_.Push(X86_RBP);
	prologue_push_end_ = code_.bytes().size();
	code_.MovRegReg(64, X86_RBP, X86_RSP);
	prologue_frame_end_ = code_.bytes().size();
	if (fn_->stack_size != 0)
		code_.AluRegImm(X86_SUB_RI, 64, X86_RSP,
		                CheckedImm32(
		                    static_cast<long long>(fn_->stack_size)));
	for (std::size_t i = 0; i < fn_->callee_saved_regs.size(); i++)
		code_.MovMemReg(64, SavedRegSlot(i),
		                static_cast<int>(fn_->callee_saved_regs[i]));
	prologue_end_ = code_.bytes().size();
}

void FunctionEncoder::EmitEpilogue()
{
	for (std::size_t i = 0; i < fn_->callee_saved_regs.size(); i++)
		code_.MovRegMem(64, static_cast<int>(fn_->callee_saved_regs[i]),
		                SavedRegSlot(i));
	code_.MovRegReg(64, X86_RSP, X86_RBP);
	code_.Pop(X86_RBP);
}

X86Mem FunctionEncoder::MemOperand(const Op & op, long long extra)
{
	switch (op.kind)
	{
	case Op::OP_FRAME:
		return X86Mem(X86_RBP, static_cast<int>(op.offset + extra));
	case Op::OP_DEREF:
		return X86Mem(Reg(op), static_cast<int>(op.offset + extra));
	case Op::OP_GLOBAL:
		return X86Mem::Absolute(X86Imm::Label(
			env_.SymbolLabel(op.text),
			static_cast<unsigned long long>(extra)));
	default:
		throw logic_error("machine IR operand is not a memory reference");
	}
}

int FunctionEncoder::ScratchGpr(const Ins & ins) const
{
	static const int candidates[6] =
	{
		X86_RAX, X86_RCX, X86_RDX, X86_RSI, X86_RDI, X86_RBX
	};
	for (int c = 0; c < 6; c++)
	{
		bool used = false;
		for (std::size_t i = 0; i < ins.operands.size(); i++)
		{
			const Op & op = ins.operands[i];
			if ((op.kind == Op::OP_REG || op.kind == Op::OP_DEREF) &&
			    Reg(op) == candidates[c])
				used = true;
		}
		if (!used)
			return candidates[c];
	}
	throw logic_error("no scratch register available");
}

// Integer and control instructions.

void FunctionEncoder::EmitMov(const Ins & ins)
{
	const Op & dst = ins.operands[0];
	const Op & src = ins.operands[1];
	if (src.kind == Op::OP_REG)
		code_.MovRegReg(64, Reg(dst), Reg(src));
	else if (src.kind == Op::OP_IMM)
		code_.MovRegImm(Reg(dst), X86Imm::Constant(
			static_cast<unsigned long long>(src.imm)));
	else if (src.kind == Op::OP_SYMBOL || src.kind == Op::OP_GLOBAL)
		code_.MovRegImm(Reg(dst),
		                X86Imm::Label(env_.SymbolLabel(src.text), 0));
	else
		throw logic_error("unsupported mov source operand");
}

void FunctionEncoder::EmitLoad(const Ins & ins)
{
	int bits = TypeBits(ins.type);
	int dst = Reg(ins.operands[0]);
	X86Mem mem = MemOperand(ins.operands[1], 0);
	if (bits <= 16)
		code_.MovzxMem(bits, dst, mem);
	else
		code_.MovRegMem(bits == 32 ? 32 : 64, dst, mem);
}

void FunctionEncoder::EmitExtend(const Ins & ins)
{
	int reg = Reg(ins.operands[0]);
	if (ins.opcode == Ins::MI_MOVZX)
	{
		code_.Movzx(8, reg, Reg(ins.operands.back()));
		return;
	}
	int bits = TypeBits(ins.type);
	if (bits >= 64)
		return;
	if (ins.opcode == Ins::MI_SEXT)
		code_.Movsx64(bits, reg, reg);
	else if (bits <= 16)
		code_.Movzx(bits, reg, reg);
	else
		code_.MovRegReg(32, reg, reg);
}

void FunctionEncoder::EmitBinary(const Ins & ins)
{
	int dst = Reg(ins.operands[0]);
	const Op & src = ins.operands[1];
	EX86Mnemonic reg_op;
	EX86Mnemonic imm_op;
	switch (ins.opcode)
	{
	case Ins::MI_ADD: reg_op = X86_ADD; imm_op = X86_ADD_RI; break;
	case Ins::MI_SUB: reg_op = X86_SUB; imm_op = X86_SUB_RI; break;
	case Ins::MI_ADC: reg_op = X86_ADC; imm_op = X86_ADC_RI; break;
	case Ins::MI_SBB: reg_op = X86_SBB; imm_op = X86_SBB_RI; break;
	case Ins::MI_AND: reg_op = X86_AND; imm_op = X86_AND_RI; break;
	case Ins::MI_OR: reg_op = X86_OR; imm_op = X86_OR_RI; break;
	case Ins::MI_XOR: reg_op = X86_XOR; imm_op = X86_XOR_RI; break;
	default:
		if (src.kind == Op::OP_REG)
			code_.ImulRegReg(dst, Reg(src));
		else
			code_.ImulRegImm(dst, CheckedImm32(src.imm));
		return;
	}
	if (src.kind == Op::OP_REG)
		code_.Alu(reg_op, 64, dst, Reg(src));
	else
		code_.AluRegImm(imm_op, 64, dst, CheckedImm32(src.imm));
}

void FunctionEncoder::EmitBswap(const Ins & ins)
{
	int reg = Reg(ins.operands[0]);
	int bits = ins.type.empty() ? 64 : TypeBits(ins.type);
	if (bits >= 64)
		code_.Bswap(64, reg);
	else if (bits == 32)
		code_.Bswap(32, reg);
	else if (bits == 16)
		code_.RorRegImm(16, reg, 8);
	// 8-bit byte swap is the identity.
}

void FunctionEncoder::EmitCompare(const Ins & ins, bool is_test)
{
	int width = TypeBits(ins.type);
	const Op & a = ins.operands[0];
	const Op & b = ins.operands[1];
	if (a.kind == Op::OP_REG)
	{
		if (b.kind == Op::OP_REG)
			code_.Alu(is_test ? X86_TEST : X86_CMP, width, Reg(a), Reg(b));
		else if (b.kind == Op::OP_IMM)
			code_.AluRegImm(is_test ? X86_TEST_RI : X86_CMP_RI, width,
			                Reg(a), CheckedImm32(b.imm));
		else  // test is symmetric; only one r/m form exists
			code_.AluRegMem(is_test ? X86_TEST : X86_CMP, width, Reg(a),
			                MemOperand(b, 0));
		return;
	}
	if (IsMemOperand(a))
	{
		if (b.kind == Op::OP_REG)
			code_.AluRegMem(is_test ? X86_TEST : X86_CMP_MR, width,
			                Reg(b), MemOperand(a, 0));
		else
			code_.AluMemImm(is_test ? X86_TEST_RI : X86_CMP_RI, width,
			                MemOperand(a, 0), CheckedImm32(b.imm));
		return;
	}
	EmitCompareImmLhs(ins, width, is_test);
}

// An immediate on the compare's left side has no direct encoding:
// stage it in a pushed scratch register (push/pop leave flags alone,
// so the staged compare's flags survive to the consumer).
void FunctionEncoder::EmitCompareImmLhs(const Ins & ins, int width,
                                        bool is_test)
{
	const Op & a = ins.operands[0];
	const Op & b = ins.operands[1];
	if (a.kind != Op::OP_IMM)
		throw logic_error("unsupported compare operand form");
	int scratch = ScratchGpr(ins);
	code_.Push(scratch);
	code_.MovRegImm(scratch,
	                X86Imm::Constant(static_cast<unsigned long long>(a.imm)));
	if (b.kind == Op::OP_REG)
		code_.Alu(is_test ? X86_TEST : X86_CMP, width, scratch, Reg(b));
	else if (b.kind == Op::OP_IMM)
		code_.AluRegImm(is_test ? X86_TEST_RI : X86_CMP_RI, width, scratch,
		                CheckedImm32(b.imm));
	else
	{
		// The push moved rsp: rsp-based operands sit 8 bytes deeper.
		X86Mem mem = MemOperand(b, 0);
		if (b.kind == Op::OP_DEREF && b.reg == XR_RSP)
			mem.disp += 8;
		code_.AluRegMem(is_test ? X86_TEST : X86_CMP, width, scratch, mem);
	}
	code_.Pop(scratch);
}

void FunctionEncoder::EmitBranch(const Ins & ins)
{
	if (ins.opcode == Ins::MI_JMP)
		code_.JmpRel32(X86Imm::Constant(0));
	else
		code_.JccRel32(static_cast<int>(ins.condition),
		               X86Imm::Constant(0));
	BlockFixup fixup;
	fixup.offset = code_.bytes().size() - 4;
	fixup.target = ins.operands[0].text;
	fixups_.push_back(fixup);
}

void FunctionEncoder::EmitTlsAddr(const Ins & ins)
{
	int dst = Reg(ins.operands[0]);
	const std::string & wrapper = ins.operands[1].text;
	// PA32 host TLS: every access goes through the per-TU wrapper
	// (synthesized below); the private executable model keeps the
	// single-threaded direct-global fallback.
	if (env_.HasFunction(wrapper) || env_.program().host_tls)
	{
		code_.CallRel32(X86Imm::Label(env_.SymbolLabel(wrapper), 0));
		if (dst != X86_RAX)
			code_.MovRegReg(64, dst, X86_RAX);
		return;
	}
	code_.MovRegImm(dst, X86Imm::Label(
		env_.SymbolLabel(env_.TlsBackingGlobal(wrapper)), 0));
}

// Landing-pad entry. Both unwinders arrive with the exception cookie
// in rax and the selector in edx, and only rbp reliably restored:
// re-establish the frame's steady-state rsp (a no-op under the host
// unwinder except after stack-argument call sites, the whole context
// under the private frame-chain walker), then spill the landing
// registers into the per-frame host-EH slots.
void FunctionEncoder::EmitEhLanding()
{
	if (!fn_->host_eh_enabled)
		throw logic_error("eh_landing outside a host-EH function");
	code_.Lea(X86_RSP,
	          X86Mem(X86_RBP,
	                 -static_cast<int>(fn_->stack_size)));
	code_.MovMemReg(64,
	                X86Mem(X86_RBP,
	                       static_cast<int>(
	                           fn_->host_eh_exception_offset)),
	                X86_RAX);
	code_.MovMemReg(32,
	                X86Mem(X86_RBP,
	                       static_cast<int>(
	                           fn_->host_eh_selector_offset)),
	                X86_RDX);
}

void FunctionEncoder::EmitInstruction(const Ins & ins)
{
	switch (ins.opcode)
	{
	case Ins::MI_MOV: EmitMov(ins); return;
	case Ins::MI_LEA:
		code_.Lea(Reg(ins.operands[0]), MemOperand(ins.operands[1], 0));
		return;
	case Ins::MI_LOAD: EmitLoad(ins); return;
	case Ins::MI_STORE:
	{
		int bits = TypeBits(ins.type);
		code_.MovMemReg(bits > 64 ? 64 : bits,
		                MemOperand(ins.operands[0], 0),
		                Reg(ins.operands[1]));
		return;
	}
	case Ins::MI_SEXT:
	case Ins::MI_ZEXT:
	case Ins::MI_MOVZX: EmitExtend(ins); return;
	case Ins::MI_ADD:
	case Ins::MI_ADC:
	case Ins::MI_SUB:
	case Ins::MI_SBB:
	case Ins::MI_AND:
	case Ins::MI_OR:
	case Ins::MI_XOR:
	case Ins::MI_IMUL: EmitBinary(ins); return;
	case Ins::MI_MUL:
		code_.MulDiv(X86_MUL, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_NEG: code_.NegReg(64, Reg(ins.operands[0])); return;
	case Ins::MI_NOT: code_.NotReg(64, Reg(ins.operands[0])); return;
	case Ins::MI_BSWAP: EmitBswap(ins); return;
	case Ins::MI_CMP: EmitCompare(ins, false); return;
	case Ins::MI_TEST: EmitCompare(ins, true); return;
	case Ins::MI_SETCC:
		code_.Setcc(static_cast<int>(ins.condition),
		            Reg(ins.operands[0]));
		return;
	case Ins::MI_JCC:
	case Ins::MI_JMP: EmitBranch(ins); return;
	case Ins::MI_CQO: code_.SignExtendAcc(64); return;
	case Ins::MI_IDIV:
		code_.MulDiv(X86_IDIV, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_DIV:
		code_.MulDiv(X86_DIV, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_SHL_CL:
		code_.ShiftRegCl(X86_SHL, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_SHR_CL:
		code_.ShiftRegCl(X86_SHR, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_SAR_CL:
		code_.ShiftRegCl(X86_SAR, 64, Reg(ins.operands[0]));
		return;
	case Ins::MI_CALL:
	{
		std::size_t start = code_.bytes().size();
		code_.CallRel32(X86Imm::Label(
			env_.SymbolLabel(ins.operands[0].text), 0));
		if (ins.eh_region >= 0)
		{
			NativeEhCallSite site;
			site.start = start;
			site.end = code_.bytes().size();
			site.region = ins.eh_region;
			eh_call_sites_.push_back(site);
		}
		return;
	}
	case Ins::MI_CALL_INDIRECT:
	{
		std::size_t start = code_.bytes().size();
		code_.CallReg(Reg(ins.operands[0]));
		if (ins.eh_region >= 0)
		{
			NativeEhCallSite site;
			site.start = start;
			site.end = code_.bytes().size();
			site.region = ins.eh_region;
			eh_call_sites_.push_back(site);
		}
		return;
	}
	case Ins::MI_JMP_INDIRECT: code_.JmpReg(Reg(ins.operands[0])); return;
	case Ins::MI_EH_LANDING: EmitEhLanding(); return;
	case Ins::MI_RET:
		EmitEpilogue();
		code_.Ret();
		return;
	case Ins::MI_EXIT:
		code_.MovRegImm(X86_RAX, X86Imm::Constant(60));
		code_.Syscall();
		return;
	case Ins::MI_COPY_BYTES:
		code_.MovRegImm(X86_RCX, X86Imm::Constant(ins.byte_count));
		code_.Fixed(X86_REP_MOVSB);
		return;
	case Ins::MI_ZERO_BYTES:
		code_.Alu(X86_XOR, 32, X86_RAX, X86_RAX);
		code_.MovRegImm(X86_RCX, X86Imm::Constant(ins.byte_count));
		code_.Fixed(X86_REP_STOSB);
		return;
	case Ins::MI_MFENCE: code_.Fixed(X86_MFENCE); return;
	case Ins::MI_XCHG:
		code_.AluRegMem(X86_XCHG_MR, TypeBits(ins.type),
		                Reg(ins.operands[1]),
		                MemOperand(ins.operands[0], 0));
		return;
	case Ins::MI_LOCK_XADD:
		code_.AluRegMem(X86_XADD_LOCK, TypeBits(ins.type),
		                Reg(ins.operands[1]),
		                MemOperand(ins.operands[0], 0));
		return;
	case Ins::MI_LOCK_CMPXCHG:
		code_.AluRegMem(X86_CMPXCHG_LOCK, TypeBits(ins.type),
		                Reg(ins.operands[1]),
		                MemOperand(ins.operands[0], 0));
		return;
	case Ins::MI_TLS_ADDR: EmitTlsAddr(ins); return;
	default: EmitFloatInstruction(ins); return;
	}
}

// Floating-point instructions and conversions.

X86Mem FunctionEncoder::PoolFloatMem(int bits, long double value)
{
	return X86Mem::Absolute(
		X86Imm::Label(env_.FloatConstantLabel(bits, value), 0));
}

X86Mem FunctionEncoder::FloatRm(int bits, const Op & op)
{
	if (op.kind == Op::OP_FLOAT_IMM)
		return PoolFloatMem(bits, op.float_imm);
	return MemOperand(op, 0);
}

// The 48-byte scratch region holds three 16-byte x87/conversion
// slots at the bottom of the frame.
X86Mem FunctionEncoder::ScratchSlot(int slot, long long extra)
{
	if (fn_ == 0 || fn_->scratch_bytes < 48)
		throw logic_error("machine IR needs scratch space the frame lacks");
	return X86Mem(X86_RBP, static_cast<int>(
		-static_cast<long long>(fn_->stack_size) + 16 * slot + extra));
}

void FunctionEncoder::CopyToXmm15(int bits, const Op & op)
{
	if (op.kind == Op::OP_XMM)
		code_.SseRegReg(MovLoad(bits), 15, Xmm(op));
	else
		code_.SseRegMem(MovLoad(bits), 15, FloatRm(bits, op));
}

void FunctionEncoder::StoreXmmTo(int bits, const Op & dst, int xmm)
{
	if (dst.kind == Op::OP_XMM)
		code_.SseRegReg(MovLoad(bits), Xmm(dst), xmm);
	else
		code_.SseRegMem(MovStore(bits), xmm, MemOperand(dst, 0));
}

void FunctionEncoder::FldOperand(int bits, const Op & op)
{
	code_.X87Mem(X86_FLD, bits, FloatRm(bits, op));
}

// Like FldOperand, but the value may sit in an xmm register and take
// a memory bounce.
void FunctionEncoder::FldFloatSource(int bits, const Op & op)
{
	if (op.kind == Op::OP_XMM)
	{
		code_.SseRegMem(MovStore(bits), Xmm(op), ScratchSlot(0, 0));
		code_.X87Mem(X86_FLD, bits, ScratchSlot(0, 0));
		return;
	}
	FldOperand(bits, op);
}

// Leave the integer flags exactly as `ucomis a, b` would: fcomip
// after pushing b then a compares st(0)=a against st(1)=b with the
// same CF/ZF/PF convention.
void FunctionEncoder::UcomisFlags(int bits, const Op & a, const Op & b)
{
	if (bits == 80)
	{
		FldOperand(80, b);
		FldOperand(80, a);
		code_.X87Stack(X86_FCOMIP);
		code_.X87Stack(X86_FSTP_ST0);
		return;
	}
	int a_reg = 15;
	if (a.kind == Op::OP_XMM)
		a_reg = Xmm(a);
	else
		CopyToXmm15(bits, a);
	if (b.kind == Op::OP_XMM)
		code_.SseRegReg(UcomisFor(bits), a_reg, Xmm(b));
	else
		code_.SseRegMem(UcomisFor(bits), a_reg, FloatRm(bits, b));
}

void FunctionEncoder::EmitFmov(const Ins & ins)
{
	int bits = TypeBits(ins.type);
	const Op & dst = ins.operands[0];
	const Op & src = ins.operands[1];
	if (bits == 80)
	{
		FldOperand(80, src);
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	if (dst.kind == Op::OP_XMM)
	{
		if (src.kind == Op::OP_XMM)
			code_.SseRegReg(MovLoad(bits), Xmm(dst), Xmm(src));
		else
			code_.SseRegMem(MovLoad(bits), Xmm(dst), FloatRm(bits, src));
		return;
	}
	if (src.kind == Op::OP_XMM)
	{
		code_.SseRegMem(MovStore(bits), Xmm(src), MemOperand(dst, 0));
		return;
	}
	CopyToXmm15(bits, src);
	code_.SseRegMem(MovStore(bits), 15, MemOperand(dst, 0));
}

void FunctionEncoder::EmitFneg(const Ins & ins)
{
	int bits = TypeBits(ins.type);
	const Op & dst = ins.operands[0];
	const Op & src = ins.operands[1];
	if (bits == 80)
	{
		FldOperand(80, src);
		code_.X87Stack(X86_FCHS);
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	CopyToXmm15(bits, src);
	unsigned char mask[16] = { 0 };
	for (int i = bits / 8 - 1; i < 16; i += bits / 8)
		mask[i] = 0x80;
	int label = env_.ByteConstantLabel(
		bits == 32 ? "negmask32" : "negmask64", mask, 16, 16);
	code_.SseRegMem(X86_XORPS, 15,
	                X86Mem::Absolute(X86Imm::Label(label, 0)));
	StoreXmmTo(bits, dst, 15);
}

// Loads one f32/f64 source onto the x87 stack, bouncing xmm-resident
// values through the given scratch slot.
void FunctionEncoder::FldFloatViaScratch(int bits, const Op & op, int slot)
{
	if (op.kind == Op::OP_XMM)
	{
		code_.SseRegMem(MovStore(bits), Xmm(op), ScratchSlot(slot, 0));
		code_.X87Mem(X86_FLD, bits, ScratchSlot(slot, 0));
		return;
	}
	code_.X87Mem(X86_FLD, bits, FloatRm(bits, op));
}

void FunctionEncoder::EmitFarith(const Ins & ins)
{
	int bits = TypeBits(ins.type);
	const Op & dst = ins.operands[0];
	const Op & a = ins.operands[1];
	const Op & b = ins.operands[2];
	if (bits == 80)
	{
		FldOperand(80, a);
		FldOperand(80, b);
		code_.X87Stack(X87Arith(ins.opcode));  // st(1) op= st(0); pop
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	// Reference parity: f64 products and quotients round through the
	// x87 extended intermediate (the checked-in program fixtures encode
	// that double rounding); sums and differences stay in SSE.
	if (bits == 64 && (ins.opcode == Ins::MI_FMUL ||
	                   ins.opcode == Ins::MI_FDIV))
	{
		FldFloatViaScratch(64, a, 0);
		FldFloatViaScratch(64, b, 1);
		code_.X87Stack(X87Arith(ins.opcode));
		if (dst.kind == Op::OP_XMM)
		{
			code_.X87Mem(X86_FSTP, 64, ScratchSlot(0, 0));
			code_.SseRegMem(MovLoad(64), Xmm(dst), ScratchSlot(0, 0));
			return;
		}
		code_.X87Mem(X86_FSTP, 64, MemOperand(dst, 0));
		return;
	}
	CopyToXmm15(bits, a);  // staging dodges dst/source aliasing
	EX86Mnemonic op = SseArith(ins.opcode, bits);
	if (b.kind == Op::OP_XMM)
		code_.SseRegReg(op, 15, Xmm(b));
	else
		code_.SseRegMem(op, 15, FloatRm(bits, b));
	StoreXmmTo(bits, dst, 15);
}

// feq/fne/flt/fgt/fle/fge: 0/1 into the full 64-bit destination with
// IEEE unordered semantics (false, except fne). Unordered compares
// set CF/ZF/PF, so above/above-or-equal conditions are already
// NaN-false; equality needs an explicit parity guard.
void FunctionEncoder::EmitFbool(const Ins & ins)
{
	int bits = TypeBits(ins.type);
	int dst = Reg(ins.operands[0]);
	const Op & a = ins.operands[1];
	const Op & b = ins.operands[2];
	int cc;
	bool swapped = false;
	switch (ins.opcode)
	{
	case Ins::MI_FEQ: cc = X86_CC_E; break;
	case Ins::MI_FNE: cc = X86_CC_NE; break;
	case Ins::MI_FLT: cc = X86_CC_A; swapped = true; break;
	case Ins::MI_FGT: cc = X86_CC_A; break;
	case Ins::MI_FLE: cc = X86_CC_AE; swapped = true; break;
	default: cc = X86_CC_AE; break;  // MI_FGE
	}
	UcomisFlags(bits, swapped ? b : a, swapped ? a : b);
	bool ne = ins.opcode == Ins::MI_FNE;
	code_.MovRegImm(dst, X86Imm::Constant(ne ? 1 : 0));  // keeps flags
	X86CodeBuffer setcc;
	setcc.Setcc(cc, dst);
	if (ins.opcode == Ins::MI_FEQ || ne)
		code_.JccRel8(0xA, static_cast<int>(setcc.bytes().size()));  // jp
	code_.AppendBuffer(setcc);
}

void FunctionEncoder::EmitFloatConstantMove(int bits, const Op & dst,
                                            long double value)
{
	if (bits == 80)
	{
		code_.X87Mem(X86_FLD, 80, PoolFloatMem(80, value));
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	if (dst.kind == Op::OP_XMM)
	{
		code_.SseRegMem(MovLoad(bits), Xmm(dst), PoolFloatMem(bits, value));
		return;
	}
	code_.SseRegMem(MovLoad(bits), 15, PoolFloatMem(bits, value));
	code_.SseRegMem(MovStore(bits), 15, MemOperand(dst, 0));
}

void FunctionEncoder::EmitSitofp(const Ins & ins)
{
	int sbits = TypeBits(ins.source_type);
	int dbits = TypeBits(ins.type);
	const Op & dst = ins.operands[0];
	const Op & src = ins.operands[1];
	if (src.kind == Op::OP_IMM)
	{
		EmitFloatConstantMove(dbits, dst, static_cast<long double>(
			SignExtendValue(src.imm, sbits)));
		return;
	}
	// GPR values are normalized, so the 64-bit convert is exact for
	// every source width.
	if (dbits == 80)
	{
		code_.MovMemReg(64, ScratchSlot(0, 0), Reg(src));
		code_.X87Mem(X86_FILD, 64, ScratchSlot(0, 0));
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	int target = dst.kind == Op::OP_XMM ? Xmm(dst) : 15;
	code_.SseRegReg(dbits == 32 ? X86_CVTSI2SS : X86_CVTSI2SD, target,
	                Reg(src));
	if (dst.kind != Op::OP_XMM)
		StoreXmmTo(dbits, dst, 15);
}

void FunctionEncoder::EmitUitofp(const Ins & ins)
{
	int sbits = TypeBits(ins.source_type);
	int dbits = TypeBits(ins.type);
	const Op & src = ins.operands[1];
	if (src.kind == Op::OP_IMM)
	{
		EmitFloatConstantMove(dbits, ins.operands[0],
		                      static_cast<long double>(
		                          ZeroExtendValue(src.imm, sbits)));
		return;
	}
	if (sbits < 64)
	{
		EmitSitofp(ins);  // already zero-extended: signed convert is exact
		return;
	}
	EmitUnsigned64ToFloat(dbits, ins.operands[0], Reg(src));
}

// fild reads the scratch slot as signed; a source with the top bit
// set converts 2^64 too low, fixed up with one exact extended-
// precision add before the single rounding at the store.
void FunctionEncoder::EmitUnsigned64ToFloat(int dbits, const Op & dst,
                                            int src_reg)
{
	code_.MovMemReg(64, ScratchSlot(0, 0), src_reg);
	code_.X87Mem(X86_FILD, 64, ScratchSlot(0, 0));
	code_.Alu(X86_TEST, 64, src_reg, src_reg);
	X86CodeBuffer fix;
	fix.X87Mem(X86_FADD_M64, 64, PoolFloatMem(64, kTwoTo64));
	code_.JccRel8(X86_CC_NS, static_cast<int>(fix.bytes().size()));
	code_.AppendBuffer(fix);
	if (dbits == 80)
	{
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	if (dst.kind == Op::OP_XMM)
	{
		code_.X87Mem(X86_FSTP, dbits, ScratchSlot(0, 0));
		code_.SseRegMem(MovLoad(dbits), Xmm(dst), ScratchSlot(0, 0));
		return;
	}
	code_.X87Mem(X86_FSTP, dbits, MemOperand(dst, 0));
}

// Swap the x87 rounding mode to truncation (RC=11) so fistp matches
// C conversion semantics; slot 1 holds the saved and modified words.
void FunctionEncoder::SetTruncationCw(int temp_gpr)
{
	code_.X87Mem(X86_FNSTCW, 16, ScratchSlot(1, 0));
	code_.MovzxMem(16, temp_gpr, ScratchSlot(1, 0));
	code_.AluRegImm(X86_OR_RI, 32, temp_gpr, 0x0C00);
	code_.MovMemReg(16, ScratchSlot(1, 8), temp_gpr);
	code_.X87Mem(X86_FLDCW, 16, ScratchSlot(1, 8));
}

void FunctionEncoder::RestoreCw()
{
	code_.X87Mem(X86_FLDCW, 16, ScratchSlot(1, 0));
}

void FunctionEncoder::EmitTruncatingFistp(int dst_reg)
{
	SetTruncationCw(dst_reg);
	code_.X87Mem(X86_FISTP, 64, ScratchSlot(0, 0));
	RestoreCw();
	code_.MovRegMem(64, dst_reg, ScratchSlot(0, 0));
}

void FunctionEncoder::EmitFptosi(const Ins & ins)
{
	int sbits = TypeBits(ins.source_type);
	int dst = Reg(ins.operands[0]);
	const Op & src = ins.operands[1];
	if (sbits != 80)
	{
		if (src.kind == Op::OP_XMM)
			code_.SseRegReg(CvttFor(sbits), dst, Xmm(src));
		else
			code_.SseRegMem(CvttFor(sbits), dst, FloatRm(sbits, src));
		return;
	}
	FldOperand(80, src);  // before clobbering dst: it may base src
	EmitTruncatingFistp(dst);
}

// Unsigned conversion: narrow destinations fit the signed 64-bit
// convert; the full 64-bit range splits around 2^63 (convert v-2^63,
// then flip bit 63 back in).
void FunctionEncoder::EmitFptoui(const Ins & ins)
{
	int sbits = TypeBits(ins.source_type);
	if (TypeBits(ins.type) < 64)
	{
		EmitFptosi(ins);
		return;
	}
	int dst = Reg(ins.operands[0]);
	const Op & src = ins.operands[1];
	if (sbits != 80)
	{
		CopyToXmm15(sbits, src);
		X86Mem bound = PoolFloatMem(sbits, kTwoTo63);
		code_.SseRegMem(UcomisFor(sbits), 15, bound);
		X86CodeBuffer small;
		small.SseRegReg(CvttFor(sbits), dst, 15);
		X86CodeBuffer big;
		big.SseRegMem(sbits == 32 ? X86_SUBSS : X86_SUBSD, 15, bound);
		big.SseRegReg(CvttFor(sbits), dst, 15);
		big.BtcRegImm(dst, 63);
		big.JmpRel8(static_cast<int>(small.bytes().size()));
		code_.JccRel8(X86_CC_B, static_cast<int>(big.bytes().size()));
		code_.AppendBuffer(big);
		code_.AppendBuffer(small);
		return;
	}
	FldOperand(80, src);
	SetTruncationCw(dst);
	X86Mem bound = PoolFloatMem(64, kTwoTo63);
	code_.X87Mem(X86_FLD, 64, bound);  // st0 = 2^63, st1 = v
	code_.X87Stack(X86_FCOMIP);        // flags: 2^63 against v
	X86CodeBuffer small;
	small.X87Mem(X86_FISTP, 64, ScratchSlot(0, 0));
	small.MovRegMem(64, dst, ScratchSlot(0, 0));
	X86CodeBuffer big;
	big.X87Mem(X86_FSUB_M64, 64, bound);
	big.X87Mem(X86_FISTP, 64, ScratchSlot(0, 0));
	big.MovRegMem(64, dst, ScratchSlot(0, 0));
	big.BtcRegImm(dst, 63);
	big.JmpRel8(static_cast<int>(small.bytes().size()));
	code_.JccRel8(X86_CC_A, static_cast<int>(big.bytes().size()));
	code_.AppendBuffer(big);
	code_.AppendBuffer(small);
	RestoreCw();
}

void FunctionEncoder::EmitFpConvert(const Ins & ins)
{
	int sbits = TypeBits(ins.source_type);
	int dbits = TypeBits(ins.type);
	const Op & dst = ins.operands[0];
	const Op & src = ins.operands[1];
	if (sbits != 80 && dbits != 80)
	{
		EX86Mnemonic cvt = sbits == 32 ? X86_CVTSS2SD : X86_CVTSD2SS;
		int target = dst.kind == Op::OP_XMM ? Xmm(dst) : 15;
		if (src.kind == Op::OP_XMM)
			code_.SseRegReg(cvt, target, Xmm(src));
		else
			code_.SseRegMem(cvt, target, FloatRm(sbits, src));
		if (dst.kind != Op::OP_XMM)
			StoreXmmTo(dbits, dst, 15);
		return;
	}
	if (dbits == 80)
	{
		FldFloatSource(sbits, src);
		code_.X87Mem(X86_FSTP, 80, MemOperand(dst, 0));
		return;
	}
	FldOperand(80, src);
	if (dst.kind == Op::OP_XMM)
	{
		code_.X87Mem(X86_FSTP, dbits, ScratchSlot(0, 0));
		code_.SseRegMem(MovLoad(dbits), Xmm(dst), ScratchSlot(0, 0));
		return;
	}
	code_.X87Mem(X86_FSTP, dbits, MemOperand(dst, 0));
}

void FunctionEncoder::EmitFloatInstruction(const Ins & ins)
{
	switch (ins.opcode)
	{
	case Ins::MI_FMOV: EmitFmov(ins); return;
	case Ins::MI_FNEG: EmitFneg(ins); return;
	case Ins::MI_FADD:
	case Ins::MI_FSUB:
	case Ins::MI_FMUL:
	case Ins::MI_FDIV: EmitFarith(ins); return;
	case Ins::MI_FEQ:
	case Ins::MI_FNE:
	case Ins::MI_FLT:
	case Ins::MI_FGT:
	case Ins::MI_FLE:
	case Ins::MI_FGE: EmitFbool(ins); return;
	case Ins::MI_FCMP:
		// Flags as if `ucomis y, x` had run on fcmp x, y.
		UcomisFlags(TypeBits(ins.type), ins.operands[1], ins.operands[0]);
		return;
	case Ins::MI_FSTP:
		code_.X87Mem(X86_FSTP, 80, MemOperand(ins.operands[0], 0));
		return;
	case Ins::MI_FRET:
		FldOperand(80, ins.operands[0]);
		EmitEpilogue();
		code_.Ret();
		return;
	case Ins::MI_SITOFP: EmitSitofp(ins); return;
	case Ins::MI_UITOFP: EmitUitofp(ins); return;
	case Ins::MI_FPTOSI: EmitFptosi(ins); return;
	case Ins::MI_FPTOUI: EmitFptoui(ins); return;
	case Ins::MI_FPEXT:
	case Ins::MI_FPTRUNC: EmitFpConvert(ins); return;
	default:
		throw logic_error("unsupported machine IR opcode");
	}
}

}  // namespace

void WriteMirProgramExecutable(const mir_model::MirProgram & program,
                               const std::string & path)
{
	ProgramEnv env(program, false);
	std::vector<ImageItem> items;

	// Item 0 (the default entry) is the startup sled; encoding the
	// functions fills the constant pool before it is materialized.
	{
		ImageItem item;
		FunctionEncoder encoder(env, 0);
		encoder.EncodeStartup(item);
		items.push_back(item);
	}
	for (std::size_t i = 0; i < program.functions.size(); i++)
	{
		ImageItem item;
		FunctionEncoder encoder(env, &program.functions[i]);
		encoder.EncodeFunction(item);
		items.push_back(item);
	}
	std::size_t global_base = items.size();
	for (std::size_t i = 0; i < program.globals.size(); i++)
	{
		ImageItem item;
		EncodeGlobal(env, program.globals[i], item);
		items.push_back(item);
	}
	std::size_t pool_base = items.size();
	for (std::size_t i = 0; i < env.pool().size(); i++)
	{
		ImageItem item;
		item.align = env.pool()[i].align;
		item.bytes = env.pool()[i].bytes;
		items.push_back(item);
	}

	ProgramImage image;
	image.SetLabelCount(env.label_count());
	for (std::size_t i = 0; i < items.size(); i++)
		image.AddItem(items[i]);
	for (std::size_t i = 0; i < program.functions.size(); i++)
		image.BindLabel(env.SymbolLabel(program.functions[i].name), 1 + i);
	for (std::size_t i = 0; i < program.globals.size(); i++)
		image.BindLabel(env.SymbolLabel(program.globals[i].name),
		                global_base + i);
	for (std::size_t i = 0; i < env.pool().size(); i++)
		image.BindLabel(env.pool()[i].label, pool_base + i);
	image.WriteExecutable(path);
}

NativeModule EncodeMirProgramModule(const mir_model::MirProgram & program)
{
	ProgramEnv env(program, true);
	NativeModule module;

	// No startup sled: a relocatable module has no entry item; the
	// linker synthesizes the program startup. Encoding the functions
	// fills the constant pool and discovers external references.
	for (std::size_t i = 0; i < program.functions.size(); i++)
	{
		ImageItem item;
		FunctionEncoder encoder(env, &program.functions[i]);
		encoder.EncodeFunction(item);
		NativeFunctionEh eh;
		eh.item = static_cast<int>(module.items.size());
		encoder.FinishEhFacts(eh);
		module.items.push_back(item);
		module.function_eh.push_back(eh);
	}
	std::size_t global_base = module.items.size();
	for (std::size_t i = 0; i < program.globals.size(); i++)
	{
		ImageItem item;
		EncodeGlobal(env, program.globals[i], item);
		item.is_thread_local = program.globals[i].thread_local_storage;
		module.items.push_back(item);
	}
	std::size_t pool_base = module.items.size();
	for (std::size_t i = 0; i < env.pool().size(); i++)
	{
		ImageItem item;
		item.align = env.pool()[i].align;
		item.bytes = env.pool()[i].bytes;
		module.items.push_back(item);
	}

	// PA32 host TLS: define the wrapper for every thread_local this
	// module defines or accesses (the access sites call it).
	std::vector<std::pair<int, int> > wrapper_items;
	if (program.host_tls)
	{
		for (std::map<std::string, std::string>::const_iterator it =
		         program.tls_wrappers.begin();
		     it != program.tls_wrappers.end(); ++it)
		{
			const std::string & wrapper = it->first;
			const std::string & global = it->second;
			bool global_defined = false;
			std::string init_name = global + "__tls_init";
			bool init_defined = false;
			for (std::size_t i = 0; i < program.globals.size(); i++)
				if (program.globals[i].name == global)
					global_defined = true;
			for (std::size_t i = 0; i < program.functions.size(); i++)
				if (program.functions[i].name == init_name)
					init_defined = true;
			bool wrapper_used = false;
			const std::vector<std::string> & names = env.label_names();
			for (std::size_t i = 0; i < names.size(); i++)
				if (names[i] == wrapper)
					wrapper_used = true;
			if (!global_defined && !wrapper_used)
				continue;
			std::map<std::string, std::string>::const_iterator object =
				program.tls_wrapper_objects.find(wrapper);
			int init_label = init_defined
				? env.SymbolLabel(init_name) : -1;
			int probe_label = -1;
			if (init_label < 0 && object !=
			        program.tls_wrapper_objects.end() &&
			    object->second.compare(0, 4, "_ZTW") == 0)
			{
				std::string probe = "_ZTH" + object->second.substr(4);
				probe_label = env.SymbolLabel(probe);
				module.weak_undefined_labels.push_back(probe);
			}
			int global_label = env.SymbolLabel(global);
			int wrapper_label = env.SymbolLabel(wrapper);
			module.items.push_back(EncodeTlsWrapperItem(
				init_label, probe_label, global_label));
			wrapper_items.push_back(std::make_pair(
				wrapper_label,
				static_cast<int>(module.items.size()) - 1));
			module.tls_wrapper_labels.push_back(wrapper);
		}
	}

	module.label_names = env.label_names();
	module.label_items.assign(env.label_count(), -1);
	for (std::size_t i = 0; i < program.functions.size(); i++)
		module.label_items[env.SymbolLabel(program.functions[i].name)] =
			static_cast<int>(i);
	for (std::size_t i = 0; i < program.globals.size(); i++)
		module.label_items[env.SymbolLabel(program.globals[i].name)] =
			static_cast<int>(global_base + i);
	for (std::size_t i = 0; i < env.pool().size(); i++)
		module.label_items[env.pool()[i].label] =
			static_cast<int>(pool_base + i);
	for (std::size_t i = 0; i < wrapper_items.size(); i++)
		module.label_items[wrapper_items[i].first] =
			wrapper_items[i].second;
	return module;
}
