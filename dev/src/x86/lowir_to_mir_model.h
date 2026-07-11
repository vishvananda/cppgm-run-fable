#pragma once

#include <map>
#include <string>
#include <vector>

#include "lowir/lowir_program.h"
#include "lowir/lowir_validate.h"
#include "mir_model.h"

// Shared data model for the LowIR -> machine IR lowering: per-value
// analysis facts, value locations, program-wide facts, call-argument
// classification, and the pinned register-discipline constants. The
// lowering engine itself lives in lowir_to_mir.h.

namespace lowir_to_mir {

// Spelling of a LowIR type in MIR dumps ("i64", "u16", "obj<3x1>", ...).
std::string SpellType(const LowIRType & type);

long long ParseIntLiteral(const LowIROperand & operand);
long long FrameSizeOf(const LowIRType & type);

mir_model::Operand MakeReg(X64Register reg);
mir_model::Operand MakeXmm(XmmRegister xmm);
mir_model::Operand MakeImm(long long value);
mir_model::Operand MakeSymbol(const std::string & name, bool global);
mir_model::Operand MakeLabel(const std::string & label);
mir_model::Operand MakeDeref(X64Register reg, long long offset);
mir_model::Operand MakeFloatImm(const LowIROperand & literal,
                                const LowIRType & type);

struct ValueUse
{
	enum Kind
	{
		USE_STORE_VALUE,   // store <ty> value, ...
		USE_RETURN_VALUE,  // return <ty> value
		USE_COPY_SOURCE,   // %d = copy <ty> value
		USE_CALL_ARG,      // call argument
		USE_BINARY_LHS,    // first operand of binary/cmp/unary/index/convert
		USE_OTHER          // addresses, branch conditions, everything else
	};

	int position = 0;
	Kind kind = USE_OTHER;
	int arg_index = -1;   // for USE_CALL_ARG
};

struct ValueInfo
{
	LowIRType type;
	bool is_param = false;
	int param_index = -1;
	int raw_references = 0;   // textual operand occurrences before rewrites
	int def_position = -1;
	std::vector<ValueUse> uses;
	bool cross_block = false;
	bool crosses_call = false;   // a call sits strictly inside the live range
	bool needs_frame = false;    // the value's own address is required

	int last_use() const;   // latest of def_position and all use positions
};

// Where a value currently lives.
struct ValueLocation
{
	enum Kind
	{
		VL_NONE,        // dead or not yet defined
		VL_GPR,
		VL_XMM,
		VL_FRAME,       // frame home at frame_offset
		VL_ARG_REG,     // forwarded parameter still in its incoming register
		VL_PENDING_COPY,// scratch param copy not yet materialized
		VL_SLOT_ADDR    // addr-of-slot temp, rematerialized at each use
	};

	Kind kind = VL_NONE;
	X64Register reg = XR_RAX;
	XmmRegister xmm = XMM_0;
	long long frame_offset = 0;
	std::string slot_name;
	bool also_in_rax = false;   // call/setcc results linger in rax
	bool prefer_home = false;   // param copy whose home is read while valid
	bool pending_r9_first = true;   // lazy copy scan class
};

struct SlotInfo
{
	LowIRType type;
	bool promoted = false;       // rewritten to register copies
	std::string alias_param;     // object slot aliased to a param home
	long long frame_offset = 0;
};

// Everything program-wide the per-function lowering needs to see.
struct ProgramFacts
{
	const LowIRProgramInfo * info = 0;
	// thread-local global name -> declared wrapper symbol
	std::map<std::string, std::string> tls_wrapper_of_global;
};

// Shared LowIR fact predicates (single owners; see lowir_to_mir_value.cpp
// and lowir_to_mir_analyze.cpp for the definitions).
std::string ContainerSpelling(long long bytes);
bool FitsImm32(long long value);
bool ParamPassWantsAddress(const LowIRParam & param);
bool StorageIsTls(const LowIROperand & operand, const ProgramFacts & facts);
bool InstructionEmbedsCall(const LowIRInstruction & ins,
                           const ProgramFacts & facts);
const std::vector<LowIRParam> * FindCalleeParams(const LowIRInstruction & call,
                                                 const ProgramFacts & facts);

// One call argument's ABI placement. ClassifyCallArgs is the single owner
// of the caller-side argument classification; both staging (LowerCall) and
// analysis (CallArgTargetsHome) consume it so they can never disagree.
struct ArgSlot
{
	enum Kind { AS_GPR, AS_XMM, AS_STACK } kind = AS_GPR;
	int ordinal = 0;
	long long stack_offset = 0;
	long long stack_bytes = 8;   // stack slots only: padded region size
	LowIRType param_type;
	bool by_address = false;
};

std::vector<ArgSlot> ClassifyCallArgs(
	const std::vector<LowIRParam> & params, const LowIRInstruction & ins,
	const std::map<std::string, ValueInfo> & values);

// Register-discipline constants (defined in lowir_to_mir_value.cpp): the
// SysV integer argument registers in ABI order, and the GPR allocation
// pool — scratch first, callee-saved from kCalleeSavedStart on.
const int kPoolSize = 7;
extern const X64Register kArgRegs[6];
extern const X64Register kPool[kPoolSize];
extern const int kCalleeSavedStart;

}  // namespace lowir_to_mir
