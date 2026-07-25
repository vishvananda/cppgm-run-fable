#include "x86/lowir_to_mir.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

// Pre-lowering analysis: linearize the block list, promote trivially
// register-shaped slots, compute value live ranges and use kinds, and plan
// the parameter register assignments (see lowir_to_mir.h for the pinned
// register discipline these implement).

namespace lowir_to_mir {

std::string SpellType(const LowIRType & type)
{
	switch(type.kind) {
		case LOWIR_TYPE_VOID: return "void";
		case LOWIR_TYPE_I1: return "i1";
		case LOWIR_TYPE_I8: return "i8";
		case LOWIR_TYPE_U8: return "u8";
		case LOWIR_TYPE_I16: return "i16";
		case LOWIR_TYPE_U16: return "u16";
		case LOWIR_TYPE_I32: return "i32";
		case LOWIR_TYPE_U32: return "u32";
		case LOWIR_TYPE_I64: return "i64";
		case LOWIR_TYPE_I128: return "i128";
		case LOWIR_TYPE_F32: return "f32";
		case LOWIR_TYPE_F64: return "f64";
		case LOWIR_TYPE_F80: return "f80";
		case LOWIR_TYPE_PTR: return "ptr";
		case LOWIR_TYPE_OBJ: {
			std::ostringstream out;
			out << "obj<" << type.obj_bytes << "x" << type.obj_align << ">";
			return out.str();
		}
	}
	throw std::logic_error("unspellable LowIR type");
}

FunctionLowering::FunctionLowering(const LowIRFunction & function,
                                   const ProgramFacts & facts,
                                   mir_model::MirFunction & out)
	: function_(function), facts_(facts), out_(out)
{
	for(int i = 0; i < kPoolSize; i++) {
		pool_holder_[i] = "";
		pool_clobbered_[i] = false;
	}
	for(int i = 0; i < 8; i++)
		xmm_holder_[i] = "";
}

void FunctionLowering::Linearize()
{
	for(size_t b = 0; b < function_.blocks.size(); b++) {
		block_first_position_.push_back((int)linear_.size());
		const LowIRBlock & block = function_.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); i++)
			linear_.push_back(&block.instructions[i]);
	}
	block_first_position_.push_back((int)linear_.size());
}

// Storage accesses that lower through the TLS wrapper call, so operands
// live *into* them must be call-safe.
bool StorageIsTls(const LowIROperand & operand, const ProgramFacts & facts)
{
	return operand.kind == LOWIR_OPERAND_GLOBAL &&
	       facts.tls_wrapper_of_global.count(operand.name) != 0;
}

bool InstructionEmbedsCall(const LowIRInstruction & ins,
                           const ProgramFacts & facts)
{
	if(ins.opcode == LOWIR_INS_LOAD || ins.opcode == LOWIR_INS_STORE ||
	   ins.opcode == LOWIR_INS_ADDR) {
		for(size_t o = 0; o < ins.operands.size(); o++)
			if(StorageIsTls(ins.operands[o], facts))
				return true;
	}
	return false;
}

// The declared parameter list a call site is staged against (null for
// calls with neither a known callee nor an explicit signature).
const std::vector<LowIRParam> * FindCalleeParams(const LowIRInstruction & call,
                                                 const ProgramFacts & facts)
{
	if(!call.callee_is_temp && facts.info->is_function(call.callee))
		return &facts.info->functions.find(call.callee)->second->params;
	if(call.signature.present)
		return &call.signature.params;
	return 0;
}

// block_first_position_ is sorted and ends with a linear_.size() sentinel.
int FunctionLowering::enclosing_block_begin(int position) const
{
	std::vector<int>::const_iterator after = std::upper_bound(
		block_first_position_.begin(), block_first_position_.end(),
		position);
	return after == block_first_position_.begin() ? 0 : *(after - 1);
}

int FunctionLowering::enclosing_block_end(int position) const
{
	std::vector<int>::const_iterator after = std::upper_bound(
		block_first_position_.begin(), block_first_position_.end(),
		position);
	return after == block_first_position_.end()
		? (int)linear_.size() : *after;
}

namespace {

bool block_is_reentered(const LowIRFunction & function)
{
	if(function.blocks.empty())
		return false;
	const std::string & entry = function.blocks[0].label;
	for(size_t b = 0; b < function.blocks.size(); b++) {
		const LowIRBlock & block = function.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); i++) {
			const LowIRInstruction & ins = block.instructions[i];
			for(size_t t = 0; t < ins.block_targets.size(); t++)
				if(ins.block_targets[t] == entry)
					return true;
		}
	}
	return false;
}

bool same_type(const LowIRType & a, const LowIRType & b)
{
	return a.kind == b.kind && a.obj_bytes == b.obj_bytes &&
	       a.obj_align == b.obj_align;
}

}  // namespace

// A slot is promoted to register copies when it behaves like a simple
// value home: address never taken, written exactly once in the (never
// re-entered) entry block before every read, and always accessed at its
// own type. The store disappears and each load becomes a copy of the
// stored operand.
void FunctionLowering::PromoteSlots()
{
	for(size_t s = 0; s < function_.slots.size(); s++)
		slots_[function_.slots[s].name].type = function_.slots[s].type;
	if(function_.blocks.empty() || block_is_reentered(function_))
		return;

	// One linear pass: a per-slot access summary, plus a prefix count of
	// call-like instructions for the store-to-load range check.
	struct SlotScan
	{
		int store_position = -1;
		int store_count = 0;
		int first_load = -1;
		int last_load = -1;
		bool blocked = false;
	};
	std::map<std::string, SlotScan> scans;
	std::vector<int> calls_before(linear_.size() + 1, 0);
	for(size_t p = 0; p < linear_.size(); p++) {
		const LowIRInstruction & ins = *linear_[p];
		calls_before[p + 1] = calls_before[p] +
			(ins.opcode == LOWIR_INS_CALL ||
			 InstructionEmbedsCall(ins, facts_) ? 1 : 0);
		if(ins.opcode == LOWIR_INS_ADDR &&
		   ins.operands[0].kind == LOWIR_OPERAND_SLOT)
			scans[ins.operands[0].name].blocked = true;
		else if(ins.opcode == LOWIR_INS_STORE &&
		        ins.operands[1].kind == LOWIR_OPERAND_SLOT) {
			SlotScan & scan = scans[ins.operands[1].name];
			scan.store_position = (int)p;
			scan.store_count++;
			if(!same_type(ins.type, slots_[ins.operands[1].name].type))
				scan.blocked = true;
		}
		else if(ins.opcode == LOWIR_INS_LOAD &&
		        ins.operands[0].kind == LOWIR_OPERAND_SLOT) {
			SlotScan & scan = scans[ins.operands[0].name];
			if(scan.first_load < 0)
				scan.first_load = (int)p;
			scan.last_load = (int)p;
			if(!same_type(ins.type, slots_[ins.operands[0].name].type))
				scan.blocked = true;
		}
		else if(ins.opcode != LOWIR_INS_LOAD &&
		        ins.opcode != LOWIR_INS_STORE) {
			for(size_t o = 0; o < ins.operands.size(); o++)
				if(ins.operands[o].kind == LOWIR_OPERAND_SLOT)
					scans[ins.operands[o].name].blocked = true;
		}
	}

	int entry_end = block_first_position_[1];
	for(std::map<std::string, SlotScan>::iterator it = scans.begin();
	    it != scans.end(); ++it) {
		const SlotScan & scan = it->second;
		SlotInfo & slot = slots_[it->first];
		if(scan.blocked || scan.store_count != 1 ||
		   scan.store_position >= entry_end ||
		   (scan.first_load >= 0 &&
		    scan.store_position > scan.first_load) ||
		   FrameSizeOf(slot.type) != 8)
			continue;
		if(scan.last_load >= 0 &&
		   calls_before[scan.last_load + 1] -
		       calls_before[scan.store_position + 1] > 0)
			continue;
		slot.promoted = true;
		promoted_slot_value_[it->first] =
			linear_[scan.store_position]->operands[0];
		skip_positions_.insert(scan.store_position);
	}
	AliasObjectParamSlots();
}

// An object slot filled once at entry by copying an object parameter in
// aliases the parameter's own frame home: the copy disappears and every
// address of the slot rematerializes against the parameter home.
void FunctionLowering::AliasObjectParamSlots()
{
	if(function_.blocks.empty() || block_is_reentered(function_))
		return;
	// textual temp-operand reference counts, computed once up front
	std::map<std::string, int> temp_references;
	for(size_t q = 0; q < linear_.size(); q++) {
		const LowIRInstruction & ins = *linear_[q];
		for(size_t o = 0; o < ins.operands.size(); o++)
			if(ins.operands[o].kind == LOWIR_OPERAND_TEMP)
				temp_references[ins.operands[o].name]++;
	}
	int entry_end = block_first_position_[1];
	for(int p = 0; p < entry_end; p++) {
		const LowIRInstruction & copy = *linear_[p];
		if(copy.opcode != LOWIR_INS_COPYOBJ)
			continue;
		if(copy.operands[0].kind != LOWIR_OPERAND_TEMP ||
		   copy.operands[1].kind != LOWIR_OPERAND_TEMP)
			continue;
		const LowIRParam * param = 0;
		for(size_t i = 0; i < function_.params.size(); i++)
			if(function_.params[i].name == copy.operands[0].name)
				param = &function_.params[i];
		if(!param || param->type.kind != LOWIR_TYPE_OBJ ||
		   param->type.obj_bytes != copy.span_bytes)
			continue;
		// destination must be a fresh addr-of-slot temporary
		const std::string & dest = copy.operands[1].name;
		std::string slot_name;
		for(int q = 0; q < p; q++) {
			const LowIRInstruction & addr = *linear_[q];
			if(addr.opcode == LOWIR_INS_ADDR && addr.result == dest &&
			   addr.operands[0].kind == LOWIR_OPERAND_SLOT)
				slot_name = addr.operands[0].name;
		}
		if(slot_name.empty() || !slots_.count(slot_name) ||
		   slots_[slot_name].promoted)
			continue;
		// the parameter must have no other reads
		if(temp_references[param->name] != 1)
			continue;
		slots_[slot_name].alias_param = param->name;
		skip_positions_.insert(p);
	}
}

void FunctionLowering::RecordOperandUse(const LowIROperand & operand,
                                        int position, ValueUse::Kind kind,
                                        int arg_index)
{
	if(operand.kind != LOWIR_OPERAND_TEMP)
		return;
	ValueUse use;
	use.position = position;
	use.kind = kind;
	use.arg_index = arg_index;
	values_[operand.name].uses.push_back(use);
}

void FunctionLowering::RecordInstructionUses(const LowIRInstruction & ins,
                                             int position)
{
	switch(ins.opcode) {
		case LOWIR_INS_STORE:
		case LOWIR_INS_ATOMIC_STORE:
			RecordOperandUse(ins.operands[0], position,
			                 ValueUse::USE_STORE_VALUE, -1);
			for(size_t o = 1; o < ins.operands.size(); o++)
				RecordOperandUse(ins.operands[o], position,
				                 ValueUse::USE_OTHER, -1);
			return;
		case LOWIR_INS_RETURN:
			if(!ins.operands.empty())
				RecordOperandUse(ins.operands[0], position,
				                 ValueUse::USE_RETURN_VALUE, -1);
			return;
		case LOWIR_INS_COPY:
			RecordOperandUse(ins.operands[0], position,
			                 ValueUse::USE_COPY_SOURCE, -1);
			return;
		case LOWIR_INS_LOAD:
			if(ins.operands[0].kind == LOWIR_OPERAND_SLOT &&
			   slots_.count(ins.operands[0].name) &&
			   slots_[ins.operands[0].name].promoted) {
				RecordOperandUse(promoted_slot_value_[ins.operands[0].name],
				                 position, ValueUse::USE_COPY_SOURCE, -1);
				return;
			}
			RecordOperandUse(ins.operands[0], position,
			                 ValueUse::USE_OTHER, -1);
			return;
		case LOWIR_INS_UNARY:
		case LOWIR_INS_CONVERT:
		case LOWIR_INS_INDEX:
		case LOWIR_INS_BINARY:
		case LOWIR_INS_CMP:
			RecordOperandUse(ins.operands[0], position,
			                 ValueUse::USE_BINARY_LHS, -1);
			for(size_t o = 1; o < ins.operands.size(); o++)
				RecordOperandUse(ins.operands[o], position,
				                 ValueUse::USE_OTHER, -1);
			return;
		case LOWIR_INS_CALL: {
			if(ins.callee_is_temp) {
				LowIROperand callee;
				callee.kind = LOWIR_OPERAND_TEMP;
				callee.name = ins.callee;
				RecordOperandUse(callee, position, ValueUse::USE_OTHER, -1);
			}
			for(size_t a = 0; a < ins.operands.size(); a++)
				RecordOperandUse(ins.operands[a], position,
				                 ValueUse::USE_CALL_ARG, (int)a);
			return;
		}
		default:
			for(size_t o = 0; o < ins.operands.size(); o++)
				RecordOperandUse(ins.operands[o], position,
				                 ValueUse::USE_OTHER, -1);
			for(size_t v = 0; v < ins.switch_values.size(); v++)
				RecordOperandUse(ins.switch_values[v], position,
				                 ValueUse::USE_OTHER, -1);
			return;
	}
}

void FunctionLowering::AnalyzeValues()
{
	for(size_t p = 0; p < function_.params.size(); p++) {
		ValueInfo & info = values_[function_.params[p].name];
		info.type = function_.params[p].type;
		info.is_param = true;
		info.param_index = (int)p;
		info.def_position = -1;
	}
	for(size_t p = 0; p < linear_.size(); p++) {
		const LowIRInstruction & ins = *linear_[p];
		for(size_t o = 0; o < ins.operands.size(); o++)
			if(ins.operands[o].kind == LOWIR_OPERAND_TEMP &&
			   values_.count(ins.operands[o].name))
				values_[ins.operands[o].name].raw_references++;
		if(skip_positions_.count((int)p))
			continue;
		if(!ins.result.empty()) {
			ValueInfo & info = values_[ins.result];
			info.type = ins.type;
			// pointer-producing and truth-producing results carry their
			// own value type, not the instruction's element type
			if(ins.opcode == LOWIR_INS_ADDR ||
			   ins.opcode == LOWIR_INS_INDEX ||
			   (ins.opcode == LOWIR_INS_UNARY && ins.operation == "decay"))
				info.type.kind = LOWIR_TYPE_PTR;
			if(ins.opcode == LOWIR_INS_CMP)
				info.type.kind = LOWIR_TYPE_I64;
			info.def_position = (int)p;
		}
		RecordInstructionUses(ins, (int)p);
		// EH markers, plain throws, and resumes lower to runtime calls
		// (__cppgm_eh_match / __cppgm_unwind_raise), so values crossing
		// them need call-surviving homes like any other call.
		if(ins.opcode == LOWIR_INS_CALL ||
		   ins.opcode == LOWIR_INS_EH_MARKER ||
		   ins.opcode == LOWIR_INS_THROW ||
		   ins.opcode == LOWIR_INS_RESUME ||
		   InstructionEmbedsCall(ins, facts_))
			call_positions_.push_back((int)p);
		if(ins.type.is_float() || ins.type2.is_float() ||
		   ins.opcode == LOWIR_INS_CONVERT)
			touches_float_ = true;
		for(size_t o = 0; o < ins.operands.size(); o++)
			if(ins.operands[o].kind == LOWIR_OPERAND_LITERAL &&
			   ins.operands[o].literal_class != LOWIR_LITERAL_INT &&
			   ins.operands[o].literal_class != LOWIR_LITERAL_NULLPTR)
				touches_float_ = true;
	}
	RetimeSinkingCompares();
	MarkCallCrossings();
}

// A compare whose single consumer is the branch terminating its own block
// lowers at the branch; its operands stay live until then.
void FunctionLowering::RetimeSinkingCompares()
{
	for(size_t b = 0; b + 1 < block_first_position_.size(); b++) {
		int begin = block_first_position_[b];
		int end = block_first_position_[b + 1];
		if(end <= begin)
			continue;
		const LowIRInstruction & last = *linear_[end - 1];
		if(last.opcode != LOWIR_INS_BRANCH ||
		   last.operands[0].kind != LOWIR_OPERAND_TEMP)
			continue;
		for(int p = begin; p < end - 1; p++) {
			const LowIRInstruction & ins = *linear_[p];
			if(ins.opcode != LOWIR_INS_CMP ||
			   ins.result != last.operands[0].name)
				continue;
			ValueInfo & info = values_[ins.result];
			if(info.uses.size() != 1)
				continue;
			for(size_t o = 0; o < ins.operands.size(); o++) {
				if(ins.operands[o].kind != LOWIR_OPERAND_TEMP)
					continue;
				ValueInfo & operand = values_[ins.operands[o].name];
				for(size_t u = 0; u < operand.uses.size(); u++)
					if(operand.uses[u].position == p)
						operand.uses[u].position = end - 1;
			}
		}
	}
}

void FunctionLowering::MarkCallCrossings()
{
	// call_positions_ is in program order; the embedded flag is computed
	// once per call, not per (value, call) pair
	std::vector<bool> embedded(call_positions_.size(), false);
	for(size_t c = 0; c < call_positions_.size(); c++)
		embedded[c] = InstructionEmbedsCall(*linear_[call_positions_[c]],
		                                    facts_);
	for(std::map<std::string, ValueInfo>::iterator it = values_.begin();
	    it != values_.end(); ++it) {
		ValueInfo & info = it->second;
		int last = info.last_use();
		// a call strictly inside (def, last) crosses; a call *at* the last
		// use crosses only when it is embedded in that instruction
		std::vector<int>::const_iterator call = std::upper_bound(
			call_positions_.begin(), call_positions_.end(),
			info.def_position);
		if(call != call_positions_.end() &&
		   (*call < last ||
		    (*call == last && embedded[call - call_positions_.begin()])))
			info.crosses_call = true;
		int def_begin = -1, def_end = -1;
		if(info.def_position >= 0) {
			def_begin = enclosing_block_begin(info.def_position);
			def_end = enclosing_block_end(info.def_position);
		}
		for(size_t u = 0; u < info.uses.size(); u++) {
			int position = info.uses[u].position;
			if(def_begin >= 0 &&
			   (position < def_begin || position >= def_end))
				info.cross_block = true;
		}
		if(info.is_param)
			info.cross_block = true;
	}
}

// Read-only single uses at the very first lowered instruction can keep
// reading the incoming argument register; everything else gets copied out
// at entry. Parameters homed in the pool registers (r8/r9) always copy.
// Would staging argument `arg_index` of the call at `position` read this
// home register in place (the argument lands in its own incoming slot)?
bool FunctionLowering::CallArgTargetsHome(int position, int arg_index,
                                          X64Register home) const
{
	const LowIRInstruction & call = *linear_[position];
	const std::vector<LowIRParam> * params = FindCalleeParams(call, facts_);
	if(!params)
		return false;
	std::vector<ArgSlot> slots = ClassifyCallArgs(*params, call, values_,
	                                              facts_.host_object);
	if(arg_index < 0 || arg_index >= (int)slots.size())
		return false;
	return slots[arg_index].kind == ArgSlot::AS_GPR &&
	       kArgRegs[slots[arg_index].ordinal] == home;
}

bool FunctionLowering::ParamUseIsForwardable(const ValueInfo & info,
                                             X64Register home) const
{
	if(info.uses.empty())
		return true;   // dead parameter: reserve the slot, emit nothing
	if(info.crosses_call)
		return false;
	if(home == XR_R8 || home == XR_R9)
		return false;   // the incoming register is itself a pool register
	int first_position = 0;
	while(skip_positions_.count(first_position))
		first_position++;
	int entry_end = block_first_position_.size() > 1
		? block_first_position_[1] : 0;
	int last_position = first_position;
	for(size_t u = 0; u < info.uses.size(); u++) {
		const ValueUse & use = info.uses[u];
		if(use.position >= entry_end)
			return false;
		switch(use.kind) {
			case ValueUse::USE_BINARY_LHS:
				if(use.position < (int)linear_.size() &&
				   (linear_[use.position]->opcode == LOWIR_INS_CMP ||
				    linear_[use.position]->opcode == LOWIR_INS_BRANCH))
					return false;   // compares read registers directly
				break;
			case ValueUse::USE_STORE_VALUE:
			case ValueUse::USE_RETURN_VALUE:
			case ValueUse::USE_COPY_SOURCE:
				break;
			case ValueUse::USE_CALL_ARG:
				if(!CallArgTargetsHome(use.position, use.arg_index, home))
					return false;
				break;
			default:
				return false;
		}
		if(use.position > last_position)
			last_position = use.position;
	}
	// rdx/rcx double as staging registers: only an immediate first use
	// reads them in place
	if((home == XR_RDX || home == XR_RCX) &&
	   last_position != first_position)
		return false;
	// rdi/rsi survive until the use unless an intervening instruction
	// can clobber the argument registers
	for(int p = first_position; p < last_position; p++) {
		if(skip_positions_.count(p))
			continue;
		const LowIRInstruction & between = *linear_[p];
		switch(between.opcode) {
			case LOWIR_INS_CALL:
			case LOWIR_INS_COPYOBJ:
			case LOWIR_INS_ZEROINIT:
			case LOWIR_INS_ATOMIC_LOAD:
			case LOWIR_INS_ATOMIC_STORE:
			case LOWIR_INS_ATOMIC_EXCHANGE:
			case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
			case LOWIR_INS_ATOMIC_ADD_FETCH:
				return false;
			default:
				break;
		}
		if(WideOperandIsI128(between))
			return false;   // i128 staging clobbers the arg registers
		if(InstructionEmbedsCall(between, facts_))
			return false;
	}
	return true;
}

// Marks temps whose address must be materialized for a by-address call
// argument; they get frame homes at definition.
void FunctionLowering::MarkByAddressArgs()
{
	for(size_t p = 0; p < linear_.size(); p++) {
		const LowIRInstruction & ins = *linear_[p];
		if(ins.opcode != LOWIR_INS_CALL)
			continue;
		const std::vector<LowIRParam> * params =
			FindCalleeParams(ins, facts_);
		if(!params)
			continue;
		for(size_t a = 0; a < ins.operands.size() && a < params->size();
		    a++) {
			const LowIROperand & arg = ins.operands[a];
			if(arg.kind != LOWIR_OPERAND_TEMP)
				continue;
			if(!ParamPassWantsAddress((*params)[a]))
				continue;
			std::map<std::string, ValueInfo>::iterator it =
				values_.find(arg.name);
			if(it != values_.end() &&
			   it->second.type.kind != LOWIR_TYPE_PTR)
				it->second.needs_frame = true;
		}
	}
}

}  // namespace lowir_to_mir
