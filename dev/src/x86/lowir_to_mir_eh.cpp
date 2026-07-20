#include "x86/lowir_to_mir.h"

#include <sstream>
#include <stdexcept>

// PA31 host-EH region analysis.
//
// The frontend arms exception regions dynamically (eh_try/eh_cleanup
// push a region, eh_end pops it), but host objects describe regions
// statically: every call site maps to the innermost region armed
// around it, and each region's landing pad and catch clauses become
// LSDA rows the platform unwinder consults. This pass runs a forward
// dataflow of the region stack over the block graph and annotates the
// lowering so codegen can emit landing-pad entries and call-site
// facts instead of handler-record chain pushes.
//
// Join rule: the longest common bottom-up prefix. Stacks only diverge
// on the frontend's catch-miss routing (a dispatch block jumps into an
// enclosing dispatch while the outer region is still dynamically
// armed); dropping the divergent suffix matches C++ semantics - code
// reached from a handler is no longer protected by that handler's try.
//
// Throw-payload windows: between a role=eh_allocate_exception call and
// the closing role=eh_throw call, the in-flight allocation must be
// abandoned if payload construction throws. Windows with calls inside
// become synthesized cleanup regions whose landing pad resumes the
// new exception (the course runtime reclaims the arena slot; hosted
// runtimes tolerate the leak - no __cxa_free_exception surface).

namespace lowir_to_mir {

namespace {

std::string CalleeRuntimeRole(const LowIRInstruction & ins,
                              const ProgramFacts & facts)
{
	if(ins.opcode != LOWIR_INS_CALL || ins.callee_is_temp)
		return std::string();
	std::map<std::string, const LowIRFunction *>::const_iterator fn =
		facts.info->functions.find(ins.callee);
	if(fn == facts.info->functions.end())
		return std::string();
	return fn->second->metadata.find("role");
}

// Longest common bottom-up prefix of two region stacks.
std::vector<int> MeetStacks(const std::vector<int> & a,
                            const std::vector<int> & b)
{
	std::vector<int> out;
	for(size_t i = 0; i < a.size() && i < b.size(); i++) {
		if(a[i] != b[i])
			break;
		out.push_back(a[i]);
	}
	return out;
}

}  // namespace

// The catch clauses a landing pad publishes: its leading eh_catch /
// eh_catch_all / eh_filter classification markers (a filter lists the
// exception-spec's allowed types; the LSDA encoder turns it into a
// negative-filter action). A bare eh_cleanup marker means the pad
// also runs cleanup work, so the region's action chain needs a
// cleanup record (or the host personality skips the frame in phase 2
// when no catch clause matches).
std::vector<mir_model::HostEhClause> FunctionLowering::CollectEhClauses(
	const std::string & label, bool & has_cleanup) const
{
	std::map<std::string, size_t>::const_iterator found =
		block_index_of_.find(label);
	if(found == block_index_of_.end())
		throw std::runtime_error(
			"eh region targets unknown block: " + label);
	std::vector<mir_model::HostEhClause> clauses;
	const LowIRBlock & block = function_.blocks[found->second];
	for(size_t i = 0; i < block.instructions.size(); i++) {
		const LowIRInstruction & ins = block.instructions[i];
		if(ins.opcode != LOWIR_INS_EH_MARKER)
			break;
		mir_model::HostEhClause clause;
		if(ins.operation == "eh_catch") {
			clause.kind = mir_model::HostEhClause::HC_CATCH;
			clause.selector = ins.eh_selector;
			clause.type_symbol = ins.eh_types[0];
		}
		else if(ins.operation == "eh_catch_all") {
			clause.kind = mir_model::HostEhClause::HC_CATCH;
			clause.catch_all = true;
			clause.selector = ins.eh_selector;
		}
		else if(ins.operation == "eh_filter") {
			clause.kind = mir_model::HostEhClause::HC_FILTER;
			clause.filter_type_symbols = ins.eh_types;
		}
		else {
			if(ins.operation == "eh_cleanup")
				has_cleanup = true;
			continue;   // bare eh_cleanup markers
		}
		clauses.push_back(clause);
	}
	return clauses;
}

int FunctionLowering::EhRegionForArming(int position,
                                        const LowIRInstruction & ins,
                                        int parent)
{
	std::map<int, int>::iterator known = eh_region_of_position_.find(position);
	if(known != eh_region_of_position_.end()) {
		eh_regions_[known->second].parent = parent;
		return known->second;
	}
	EhRegionPlan region;
	region.parent = parent;
	if(ins.opcode == LOWIR_INS_CALL) {
		// throw-payload window: synthesized abandon-and-resume pad
		std::ostringstream label;
		label << "__eh_abandon_" << eh_regions_.size();
		region.landing_label = label.str();
		region.cleanup = true;
		region.synthetic = true;
	}
	else {
		region.landing_label = ins.block_targets[0];
		bool marker_cleanup = false;
		region.clauses = CollectEhClauses(region.landing_label,
		                                  marker_cleanup);
		region.cleanup = ins.opcode == LOWIR_INS_EH_CLEANUP ||
		                 marker_cleanup;
		eh_landing_blocks_.insert(region.landing_label);
	}
	eh_regions_.push_back(region);
	eh_region_of_position_[position] = (int)eh_regions_.size() - 1;
	return (int)eh_regions_.size() - 1;
}

// One abstract-interpretation pass over a block: maintain the region
// stack, record per-call innermost regions, and hand successor and
// landing-pad blocks their entry states.
void FunctionLowering::SimulateEhBlock(size_t block_index,
                                       std::vector<int> stack,
                                       std::vector<std::vector<int> > & in,
                                       std::vector<bool> & known,
                                       std::vector<size_t> & worklist)
{
	const LowIRBlock & block = function_.blocks[block_index];
	int position = block_first_position_[block_index];
	for(size_t i = 0; i < block.instructions.size(); i++, position++) {
		const LowIRInstruction & ins = block.instructions[i];
		switch(ins.opcode) {
			case LOWIR_INS_EH_TRY:
			case LOWIR_INS_EH_CLEANUP: {
				if(ins.block_targets.empty())
					break;   // bare cleanup marker
				int parent = stack.empty() ? -1 : stack.back();
				int region = EhRegionForArming(position, ins, parent);
				MergeEhBlockState(eh_regions_[region].landing_label,
				                  stack, in, known, worklist);
				stack.push_back(region);
				break;
			}
			case LOWIR_INS_EH_END:
				if(!stack.empty())
					stack.pop_back();
				break;
			case LOWIR_INS_CALL: {
				std::string role = CalleeRuntimeRole(ins, facts_);
				if((role == "eh_throw" || role == "eh_rethrow") &&
				   !stack.empty() &&
				   eh_regions_[stack.back()].synthetic)
					stack.pop_back();
				eh_region_at_[position] =
					stack.empty() ? -1 : stack.back();
				if(role == "eh_allocate_exception") {
					int parent = stack.empty() ? -1 : stack.back();
					stack.push_back(
						EhRegionForArming(position, ins, parent));
				}
				break;
			}
			case LOWIR_INS_THROW:
			case LOWIR_INS_RESUME:
				eh_region_at_[position] =
					stack.empty() ? -1 : stack.back();
				break;
			case LOWIR_INS_JUMP:
			case LOWIR_INS_BRANCH:
			case LOWIR_INS_SWITCH:
				for(size_t t = 0; t < ins.block_targets.size(); t++)
					MergeEhBlockState(ins.block_targets[t], stack, in,
					                  known, worklist);
				break;
			default:
				break;
		}
	}
}

void FunctionLowering::MergeEhBlockState(
	const std::string & label, const std::vector<int> & stack,
	std::vector<std::vector<int> > & in, std::vector<bool> & known,
	std::vector<size_t> & worklist)
{
	std::map<std::string, size_t>::const_iterator found =
		block_index_of_.find(label);
	if(found == block_index_of_.end())
		throw std::runtime_error(
			"eh state flows to unknown block: " + label);
	size_t b = found->second;
	if(!known[b]) {
		known[b] = true;
		in[b] = stack;
		worklist.push_back(b);
		return;
	}
	std::vector<int> met = MeetStacks(in[b], stack);
	if(met != in[b]) {
		in[b] = met;
		worklist.push_back(b);
	}
}

// Region dataflow over the function. Landing-pad blocks enter with the
// stack that was armed at their arming site (without the landed region
// itself); everything else propagates along the block terminators.
void FunctionLowering::AnalyzeEhRegions()
{
	if(function_.blocks.empty())
		return;
	for(size_t b = 0; b < function_.blocks.size(); b++)
		block_index_of_[function_.blocks[b].label] = b;
	std::vector<std::vector<int> > in(function_.blocks.size());
	std::vector<bool> known(function_.blocks.size(), false);
	std::vector<size_t> worklist;
	known[0] = true;
	worklist.push_back(0);
	while(!worklist.empty()) {
		size_t b = worklist.back();
		worklist.pop_back();
		SimulateEhBlock(b, in[b], in, known, worklist);
	}

	// Prune payload windows no call ever landed in: their abandon
	// pads would otherwise emit dead resume code (and a host
	// _Unwind_Resume reference) in throw-only functions.
	std::vector<bool> referenced(eh_regions_.size(), false);
	for(std::map<int, int>::const_iterator it = eh_region_at_.begin();
	    it != eh_region_at_.end(); ++it)
		for(int r = it->second; r >= 0 && !referenced[(size_t)r];
		    r = eh_regions_[(size_t)r].parent)
			referenced[(size_t)r] = true;
	std::vector<int> remap(eh_regions_.size(), -1);
	std::vector<EhRegionPlan> kept;
	for(size_t r = 0; r < eh_regions_.size(); r++) {
		if(eh_regions_[r].synthetic && !referenced[r])
			continue;
		remap[r] = (int)kept.size();
		kept.push_back(eh_regions_[r]);
	}
	for(size_t r = 0; r < kept.size(); r++)
		if(kept[r].parent >= 0)
			kept[r].parent = remap[(size_t)kept[r].parent];
	for(std::map<int, int>::iterator it = eh_region_at_.begin();
	    it != eh_region_at_.end(); ++it)
		if(it->second >= 0)
			it->second = remap[(size_t)it->second];
	eh_regions_.swap(kept);

	// Landing pads are unwinder entries: the register state they
	// re-establish must not be bypassed by ordinary control flow.
	for(size_t b = 0; b < function_.blocks.size(); b++) {
		const LowIRBlock & block = function_.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); i++) {
			const LowIRInstruction & ins = block.instructions[i];
			if(ins.opcode != LOWIR_INS_JUMP &&
			   ins.opcode != LOWIR_INS_BRANCH &&
			   ins.opcode != LOWIR_INS_SWITCH)
				continue;
			for(size_t t = 0; t < ins.block_targets.size(); t++)
				if(eh_landing_blocks_.count(ins.block_targets[t]))
					throw std::runtime_error(
						"eh landing pad is also a jump target: " +
						ins.block_targets[t]);
		}
	}
	if(!function_.blocks.empty() &&
	   eh_landing_blocks_.count(function_.blocks[0].label))
		throw std::runtime_error("eh landing pad is the entry block");
}

int FunctionLowering::eh_region_for(int position) const
{
	std::map<int, int>::const_iterator it = eh_region_at_.find(position);
	return it == eh_region_at_.end() ? -1 : it->second;
}

// Landing-pad entry: the unwinder (host or private) arrives with the
// exception cookie in rax and the selector in edx, and only rbp
// reliably restored. Re-establish the steady-state rsp and spill the
// landing registers to the per-frame host-EH slots that the
// exception/exception_selector reads consume.
void FunctionLowering::EmitEhLandingEntry()
{
	emit(mir_model::Instruction::MI_EH_LANDING);
	invalidate_rax();
	arg_homes_clobbered_ = true;
}

// Synthesized throw-payload abandon pads: land, then resume the new
// exception into the enclosing region (annotated so the resume call
// site itself gets the enclosing row).
void FunctionLowering::AppendEhAbandonPads()
{
	for(size_t r = 0; r < eh_regions_.size(); r++) {
		if(!eh_regions_[r].synthetic)
			continue;
		mir_model::MirBlock pad;
		pad.label = eh_regions_[r].landing_label;
		mir_model::Instruction landing;
		landing.opcode = mir_model::Instruction::MI_EH_LANDING;
		pad.instructions.push_back(landing);
		mir_model::Instruction load;
		load.opcode = mir_model::Instruction::MI_LOAD;
		load.type = "i64";
		load.operands.push_back(MakeReg(XR_RDI));
		load.operands.push_back(
			frame_operand(out_.host_eh_exception_offset));
		pad.instructions.push_back(load);
		mir_model::Instruction call;
		call.opcode = mir_model::Instruction::MI_CALL;
		call.eh_region = eh_regions_[r].parent;
		call.operands.push_back(MakeSymbol("_Unwind_Resume", false));
		pad.instructions.push_back(call);
		out_.blocks.push_back(pad);
	}
}

// Publish the typed region facts on the MIR function.
void FunctionLowering::FinishEhRegions()
{
	out_.host_eh_regions.clear();
	for(size_t r = 0; r < eh_regions_.size(); r++) {
		mir_model::HostEhRegion region;
		region.landing_label = eh_regions_[r].landing_label;
		region.cleanup = eh_regions_[r].cleanup;
		region.parent = eh_regions_[r].parent;
		region.clauses = eh_regions_[r].clauses;
		out_.host_eh_regions.push_back(region);
	}
}

}  // namespace lowir_to_mir
