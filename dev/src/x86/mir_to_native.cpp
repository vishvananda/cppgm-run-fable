#include "x86/mir_to_native.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "x86/elf_program.h"

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

// MIR type spelling -> operand width in bits (f80 -> 80).
int TypeBits(const std::string & type)
{
	if (type == "i1" || type == "i8" || type == "u8")
		return 8;
	if (type == "i16" || type == "u16")
		return 16;
	if (type == "i32" || type == "u32" || type == "f32")
		return 32;
	if (type == "f80")
		return 80;
	// i64/u64/ptr/f64 and the untyped default are 64-bit wide; anything
	// else is a producer bug and must not degrade silently.
	if (type == "i64" || type == "u64" || type == "ptr" || type == "f64" ||
	    type.empty())
		return 64;
	throw logic_error("unknown machine IR type spelling: " + type);
}

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

void AppendLittleEndian(std::vector<unsigned char> & out,
                        unsigned long long value, std::size_t size)
{
	for (std::size_t i = 0; i < size; i++)
		out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}

// Storage bytes of a value in the named float format: f32/f64 use
// their IEEE image, f80 the ten x87 bytes padded to sixteen.
void AppendFloatBits(std::vector<unsigned char> & out,
                     const std::string & type, long double value)
{
	unsigned char raw[16];
	std::memset(raw, 0, sizeof(raw));
	if (type == "f32")
	{
		float narrow = static_cast<float>(value);
		std::memcpy(raw, &narrow, 4);
		out.insert(out.end(), raw, raw + 4);
	}
	else if (type == "f64")
	{
		double narrow = static_cast<double>(value);
		std::memcpy(raw, &narrow, 8);
		out.insert(out.end(), raw, raw + 8);
	}
	else
	{
		std::memcpy(raw, &value, 10);
		out.insert(out.end(), raw, raw + 16);
	}
}

// Parse at the type's own width: rounding text through long double
// first can double-round (623e+100 lands one f64 ulp off strtod).
long double ParseFloatLiteral(const std::string & type,
                              const std::string & text,
                              long double fallback)
{
	if (text.empty())
		return fallback;
	if (type == "f32")
		return strtof(text.c_str(), 0);
	if (type == "f64")
		return strtod(text.c_str(), 0);
	return strtold(text.c_str(), 0);
}

std::size_t ScalarSize(const std::string & type)
{
	if (type == "f80")
		return 16;
	return static_cast<std::size_t>(TypeBits(type)) / 8;
}

std::size_t DataItemAlign(const Global::DataItem & item)
{
	switch (item.kind)
	{
	case Global::DataItem::ITEM_INTEGER:
	case Global::DataItem::ITEM_FLOAT:
		return ScalarSize(item.type);
	case Global::DataItem::ITEM_ADDR:
		return 8;
	case Global::DataItem::ITEM_ZERO:
		return 1;
	}
	throw logic_error("unknown global data item kind");
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
class ProgramEnv
{
public:
	ProgramEnv(const mir_model::MirProgram & program, bool allow_external);

	const mir_model::MirProgram & program() const { return program_; }
	int SymbolLabel(const std::string & name);
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

// Global data encoding.

void AppendAddrPatch(ImageItem & item, ProgramEnv & env,
                     const std::string & symbol, long long addend)
{
	X86Patch patch;
	patch.offset = item.bytes.size();
	patch.size = 8;
	patch.kind = X86_PATCH_ABS;
	patch.imm = X86Imm::Label(env.SymbolLabel(symbol),
	                          static_cast<unsigned long long>(addend));
	item.patches.push_back(patch);
	item.bytes.insert(item.bytes.end(), 8, 0);
}

void EncodeScalarGlobal(ProgramEnv & env, const Global & global,
                        ImageItem & item)
{
	std::size_t size = ScalarSize(global.type);
	item.align = size;
	switch (global.init_kind)
	{
	case Global::GI_ZERO:
		item.bytes.assign(size, 0);
		return;
	case Global::GI_INTEGER:
		AppendLittleEndian(
			item.bytes,
			static_cast<unsigned long long>(global.int_value), size);
		return;
	case Global::GI_FLOAT:
		AppendFloatBits(item.bytes, global.type,
		                ParseFloatLiteral(global.type, global.literal_text,
		                                  global.float_value));
		return;
	case Global::GI_ADDR:
		AppendAddrPatch(item, env, global.symbol, global.addr_addend);
		return;
	}
	throw logic_error("unknown global init kind");
}

void EncodeDataGlobal(ProgramEnv & env, const Global & global,
                      ImageItem & item)
{
	std::size_t max_align = 8;
	for (std::size_t i = 0; i < global.data_items.size(); i++)
	{
		const Global::DataItem & data = global.data_items[i];
		std::size_t align = DataItemAlign(data);
		if (align > max_align)
			max_align = align;
		while (item.bytes.size() % align != 0)
			item.bytes.push_back(0);
		switch (data.kind)
		{
		case Global::DataItem::ITEM_INTEGER:
			AppendLittleEndian(
				item.bytes,
				static_cast<unsigned long long>(data.int_value),
				ScalarSize(data.type));
			break;
		case Global::DataItem::ITEM_FLOAT:
			AppendFloatBits(item.bytes, data.type,
			                ParseFloatLiteral(data.type, data.literal_text,
			                                  data.float_value));
			break;
		case Global::DataItem::ITEM_ADDR:
			AppendAddrPatch(item, env, data.symbol, data.addr_addend);
			break;
		case Global::DataItem::ITEM_ZERO:
			item.bytes.insert(item.bytes.end(), data.zero_bytes, 0);
			break;
		}
	}
	item.align = max_align;
}

void EncodeGlobal(ProgramEnv & env, const Global & global,
                  ImageItem & item)
{
	if (global.storage_kind == Global::GS_SCALAR)
		EncodeScalarGlobal(env, global, item);
	else
		EncodeDataGlobal(env, global, item);
}

// Encodes one function (or the startup sled) into an image item.
class FunctionEncoder
{
public:
	FunctionEncoder(ProgramEnv & env, const mir_model::MirFunction * fn);

	void EncodeFunction(ImageItem & item);
	void EncodeStartup(ImageItem & item);

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
	void EmitEhPush(const Ins & ins);
	void EmitEhPop();
	X86Mem RuntimeGlobalMem(const char * name);
	void Finish(ImageItem & item);
	X86Mem SavedRegSlot(std::size_t index) const;
	X86Mem MemOperand(const Op & op, long long extra);
	int ScratchGpr(const Ins & ins) const;

	void EmitFloatInstruction(const Ins & ins);
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
	// EH dispatch addresses: (patch index, block) pairs whose imm64
	// addend receives the block's final intra-item offset in Finish.
	std::vector<std::pair<std::size_t, std::string> > address_fixups_;
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
	for (std::size_t i = 0; i < address_fixups_.size(); i++)
	{
		std::map<std::string, std::size_t>::const_iterator it =
			block_offsets_.find(address_fixups_[i].second);
		if (it == block_offsets_.end())
			throw runtime_error("machine IR EH dispatch to unknown block: " +
			                    address_fixups_[i].second);
		patches[address_fixups_[i].first].imm.addend += it->second;
	}
	for (std::size_t i = 0; i < patches.size(); i++)
	{
		// Label-less PCREL fields are the intra-function jumps we
		// resolve here; keep them away from the layout pass.
		if (patches[i].kind == X86_PATCH_PCREL && !patches[i].imm.has_label)
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
	code_.Push(X86_RBP);
	code_.MovRegReg(64, X86_RBP, X86_RSP);
	if (fn_->stack_size != 0)
		code_.AluRegImm(X86_SUB_RI, 64, X86_RSP,
		                CheckedImm32(
		                    static_cast<long long>(fn_->stack_size)));
	for (std::size_t i = 0; i < fn_->callee_saved_regs.size(); i++)
		code_.MovMemReg(64, SavedRegSlot(i),
		                static_cast<int>(fn_->callee_saved_regs[i]));
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
	if (env_.HasFunction(wrapper))
	{
		code_.CallRel32(X86Imm::Label(env_.SymbolLabel(wrapper), 0));
		if (dst != X86_RAX)
			code_.MovRegReg(64, dst, X86_RAX);
		return;
	}
	code_.MovRegImm(dst, X86Imm::Label(
		env_.SymbolLabel(env_.TlsBackingGlobal(wrapper)), 0));
}

X86Mem FunctionEncoder::RuntimeGlobalMem(const char * name)
{
	return X86Mem::Absolute(X86Imm::Label(env_.SymbolLabel(name), 0));
}

// Installs a handler record {prev, dispatch, rbp, rsp} at the head of
// the __cppgm_eh_top chain. The dispatch address is this function's
// own entry plus the block offset (back-filled in Finish); r10/r11 are
// staging registers the register discipline never assigns to values.
void FunctionEncoder::EmitEhPush(const Ins & ins)
{
	X86Mem record = MemOperand(ins.operands[0], 0);
	code_.MovRegMem(64, X86_R10, RuntimeGlobalMem("__cppgm_eh_top"));
	code_.MovMemReg(64, MemOperand(ins.operands[0], 0), X86_R10);
	code_.MovRegImm(X86_R11,
	                X86Imm::Label(env_.SymbolLabel(fn_->name), 0));
	address_fixups_.push_back(std::make_pair(code_.patches().size() - 1,
	                                         ins.operands[1].text));
	code_.MovMemReg(64, MemOperand(ins.operands[0], 8), X86_R11);
	code_.MovMemReg(64, MemOperand(ins.operands[0], 16), X86_RBP);
	code_.MovRegReg(64, X86_R11, X86_RSP);
	code_.MovMemReg(64, MemOperand(ins.operands[0], 24), X86_R11);
	code_.Lea(X86_R10, record);
	code_.MovMemReg(64, RuntimeGlobalMem("__cppgm_eh_top"), X86_R10);
}

// eh_end pops the chain head only when it belongs to this frame
// (rsp <= record < rbp): dispatch blocks may run more eh_end markers
// than regions remain, and the extras must not consume an ancestor
// frame's records. Built back-to-front so each jcc rel8 is exact.
void FunctionEncoder::EmitEhPop()
{
	X86CodeBuffer pop;
	pop.MovRegMem(64, X86_R10, X86Mem(X86_R10, 0));
	pop.MovMemReg(64, RuntimeGlobalMem("__cppgm_eh_top"), X86_R10);

	X86CodeBuffer upper;
	upper.Alu(X86_CMP, 64, X86_R10, X86_RBP);
	upper.JccRel8(X86_CC_AE, static_cast<int>(pop.bytes().size()));

	X86CodeBuffer lower;
	lower.Alu(X86_CMP, 64, X86_R10, X86_RSP);
	lower.JccRel8(X86_CC_B, static_cast<int>(upper.bytes().size() +
	                                         pop.bytes().size()));

	code_.MovRegMem(64, X86_R10, RuntimeGlobalMem("__cppgm_eh_top"));
	code_.AppendBuffer(lower);
	code_.AppendBuffer(upper);
	code_.AppendBuffer(pop);
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
	case Ins::MI_SUB:
	case Ins::MI_AND:
	case Ins::MI_OR:
	case Ins::MI_XOR:
	case Ins::MI_IMUL: EmitBinary(ins); return;
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
		code_.CallRel32(X86Imm::Label(
			env_.SymbolLabel(ins.operands[0].text), 0));
		return;
	case Ins::MI_CALL_INDIRECT: code_.CallReg(Reg(ins.operands[0])); return;
	case Ins::MI_JMP_INDIRECT: code_.JmpReg(Reg(ins.operands[0])); return;
	case Ins::MI_EH_PUSH: EmitEhPush(ins); return;
	case Ins::MI_EH_POP: EmitEhPop(); return;
	case Ins::MI_LOAD_EXCEPTION:
		code_.MovRegMem(64, X86_RAX,
		                RuntimeGlobalMem("__cppgm_eh_exception"));
		return;
	case Ins::MI_LOAD_EXCEPTION_SELECTOR:
		code_.MovRegMem(32, X86_RAX,
		                RuntimeGlobalMem("__cppgm_eh_selector"));
		return;
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
		module.items.push_back(item);
	}
	std::size_t global_base = module.items.size();
	for (std::size_t i = 0; i < program.globals.size(); i++)
	{
		ImageItem item;
		EncodeGlobal(env, program.globals[i], item);
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
	return module;
}
