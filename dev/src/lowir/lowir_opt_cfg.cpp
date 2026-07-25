// PA37 control-flow cleanup: unreachable-block removal, trivial
// jump-only threading, straight-line pair merging with exception
// structure guards, and no-op eh_try/eh_end stripping in functions
// spelled unwind=no. Also owns the shared exception-shape facts
// (region depth, handler blocks) other passes consult.

#include "lowir/lowir_opt_internal.h"

namespace {

int RegionDelta(const LowIRInstruction & ins)
{
	if(ins.opcode == LOWIR_INS_EH_TRY ||
	   ins.opcode == LOWIR_INS_EH_CLEANUP)
		return 1;
	if(ins.opcode == LOWIR_INS_EH_END)
		return -1;
	return 0;
}

int BlockExitDepth(const LowIRBlock & block, int depth_in)
{
	int depth = depth_in;
	for(size_t i = 0; i < block.instructions.size(); i++)
	{
		depth += RegionDelta(block.instructions[i]);
		if(depth < 0)
			depth = 0;
	}
	return depth;
}

}  // namespace

void ComputeEhShape(OptFunction & view)
{
	LowIRFunction & fn = *view.fn;
	size_t count = fn.blocks.size();
	view.depth_in.assign(count, -1);
	view.handler_target.assign(count, false);
	view.handler_context.assign(count, false);
	if(count == 0)
		return;

	// Depth at handler entry: the pushed handler is popped by the
	// transfer, so the handler continues at the push-site depth.
	vector<size_t> work;
	view.depth_in[0] = 0;
	work.push_back(0);
	while(!work.empty())
	{
		size_t b = work.back();
		work.pop_back();
		const LowIRBlock & block = fn.blocks[b];
		int depth = view.depth_in[b];
		for(size_t i = 0; i < block.instructions.size(); i++)
		{
			const LowIRInstruction & ins = block.instructions[i];
			if(ins.opcode == LOWIR_INS_EH_TRY ||
			   ins.opcode == LOWIR_INS_EH_CLEANUP)
			{
				map<string, size_t>::const_iterator target =
					view.block_index.find(ins.block_targets[0]);
				if(target != view.block_index.end())
				{
					view.handler_target[target->second] = true;
					if(view.depth_in[target->second] < 0)
					{
						view.depth_in[target->second] = depth;
						work.push_back(target->second);
					}
				}
			}
			depth += RegionDelta(ins);
			if(depth < 0)
				depth = 0;
		}
		if(block.instructions.empty())
			continue;
		vector<string> targets;
		TerminatorTargets(block.instructions.back(), targets);
		for(size_t t = 0; t < targets.size(); t++)
		{
			map<string, size_t>::const_iterator succ =
				view.block_index.find(targets[t]);
			if(succ == view.block_index.end())
				continue;
			if(view.depth_in[succ->second] < 0)
			{
				view.depth_in[succ->second] = depth;
				work.push_back(succ->second);
			}
		}
	}

	// Handler context: blocks reachable from handler targets along
	// normal edges (calls there never inline).
	for(size_t b = 0; b < count; b++)
		if(view.handler_target[b])
		{
			view.handler_context[b] = true;
			work.push_back(b);
		}
	while(!work.empty())
	{
		size_t b = work.back();
		work.pop_back();
		const LowIRBlock & block = fn.blocks[b];
		if(block.instructions.empty())
			continue;
		vector<string> targets;
		TerminatorTargets(block.instructions.back(), targets);
		for(size_t t = 0; t < targets.size(); t++)
		{
			map<string, size_t>::const_iterator succ =
				view.block_index.find(targets[t]);
			if(succ == view.block_index.end() ||
			   view.handler_context[succ->second])
				continue;
			view.handler_context[succ->second] = true;
			work.push_back(succ->second);
		}
	}
}

namespace {

struct CfgPass
{
	OptFunction & view;
	LowIRFunction & fn;

	explicit CfgPass(OptFunction & v) : view(v), fn(*v.fn) {}

	bool RemoveUnreachable();
	bool ThreadJumpOnly();
	bool MergePairs();
};

void CollectReachable(const LowIRFunction & fn,
                      const map<string, size_t> & block_index,
                      vector<bool> & reachable)
{
	reachable.assign(fn.blocks.size(), false);
	if(fn.blocks.empty())
		return;
	vector<size_t> work;
	reachable[0] = true;
	work.push_back(0);
	while(!work.empty())
	{
		size_t b = work.back();
		work.pop_back();
		const LowIRBlock & block = fn.blocks[b];
		vector<string> targets;
		for(size_t i = 0; i < block.instructions.size(); i++)
		{
			const LowIRInstruction & ins = block.instructions[i];
			if(ins.opcode == LOWIR_INS_EH_TRY ||
			   ins.opcode == LOWIR_INS_EH_CLEANUP)
				targets.push_back(ins.block_targets[0]);
		}
		if(!block.instructions.empty())
		{
			vector<string> succ;
			TerminatorTargets(block.instructions.back(), succ);
			targets.insert(targets.end(), succ.begin(), succ.end());
		}
		for(size_t t = 0; t < targets.size(); t++)
		{
			map<string, size_t>::const_iterator found =
				block_index.find(targets[t]);
			if(found == block_index.end() || reachable[found->second])
				continue;
			reachable[found->second] = true;
			work.push_back(found->second);
		}
	}
}

bool CfgPass::RemoveUnreachable()
{
	vector<bool> reachable;
	CollectReachable(fn, view.block_index, reachable);
	vector<LowIRBlock> kept;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		if(reachable[b])
			kept.push_back(fn.blocks[b]);
	if(kept.size() == fn.blocks.size())
		return false;
	fn.blocks.swap(kept);
	return true;
}

// Retargets every block reference from `from` to `to` in terminators.
// Exception targets are structure, not flow, and stay pinned.
void RetargetTerminators(LowIRFunction & fn, const string & from,
                         const string & to)
{
	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		if(fn.blocks[b].instructions.empty())
			continue;
		LowIRInstruction & term = fn.blocks[b].instructions.back();
		if(!term.is_terminator())
			continue;
		for(size_t t = 0; t < term.block_targets.size(); t++)
			if(term.block_targets[t] == from)
				term.block_targets[t] = to;
	}
}

bool CfgPass::ThreadJumpOnly()
{
	bool changed = false;
	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		const LowIRBlock & block = fn.blocks[b];
		if(b == 0 || block.instructions.size() != 1 ||
		   block.instructions[0].opcode != LOWIR_INS_JUMP)
			continue;
		if(view.handler_target[b])
			continue;
		const string & target = block.instructions[0].block_targets[0];
		if(target == block.label)
			continue;
		RetargetTerminators(fn, block.label, target);
		changed = true;
	}
	return changed;
}

bool CfgPass::MergePairs()
{
	// Predecessor counts over normal edges plus handler-target pins.
	map<string, int> pred_count;
	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		const LowIRBlock & block = fn.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); i++)
		{
			const LowIRInstruction & ins = block.instructions[i];
			if(ins.opcode == LOWIR_INS_EH_TRY ||
			   ins.opcode == LOWIR_INS_EH_CLEANUP)
				pred_count[ins.block_targets[0]] += 2;
		}
		if(block.instructions.empty())
			continue;
		vector<string> targets;
		TerminatorTargets(block.instructions.back(), targets);
		for(size_t t = 0; t < targets.size(); t++)
			pred_count[targets[t]]++;
	}

	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		LowIRBlock & block = fn.blocks[b];
		if(block.instructions.empty())
			continue;
		LowIRInstruction & term = block.instructions.back();
		if(term.opcode != LOWIR_INS_JUMP)
			continue;
		const string & target = term.block_targets[0];
		if(target == block.label || pred_count[target] != 1)
			continue;
		map<string, size_t>::const_iterator found =
			view.block_index.find(target);
		if(found == view.block_index.end())
			continue;
		size_t s = found->second;
		if(s == 0 || view.handler_target[s])
			continue;
		// Exception-structure guard: never merge across an open region
		// boundary.
		if(view.depth_in[b] < 0 ||
		   BlockExitDepth(block, view.depth_in[b]) != 0)
			continue;
		LowIRBlock & succ = fn.blocks[s];
		block.instructions.pop_back();
		block.instructions.insert(block.instructions.end(),
		                          succ.instructions.begin(),
		                          succ.instructions.end());
		// The successor's only predecessor was the merged jump, so the
		// block is gone outright; the caller rebuilds the view.
		fn.blocks.erase(fn.blocks.begin() + static_cast<long>(s));
		return true;
	}
	return false;
}

}  // namespace

bool CleanupControlFlow(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	bool changed = false;
	for(;;)
	{
		CfgPass pass(view);
		bool round = false;
		round |= pass.RemoveUnreachable();
		BuildOptFunction(view, *view.fn, *view.context);
		CfgPass threaded(view);
		round |= threaded.ThreadJumpOnly();
		BuildOptFunction(view, *view.fn, *view.context);
		CfgPass merged(view);
		round |= merged.MergePairs();
		BuildOptFunction(view, *view.fn, *view.context);
		if(!round)
			break;
		changed = true;
	}
	return changed;
}

namespace {

// Walks a protected region from the instruction after eh_try until the
// matching eh_end; false when the region shape is too irregular.
struct RegionScan
{
	OptFunction & view;
	set<std::pair<size_t, size_t> > body;    // instructions inside
	set<std::pair<size_t, size_t> > ends;    // matching eh_end sites
	bool valid = true;
	int max_depth = 0;                       // total region pushes

	explicit RegionScan(OptFunction & v) : view(v)
	{
		for(size_t b = 0; b < v.fn->blocks.size(); b++)
			for(size_t k = 0; k < v.fn->blocks[b].instructions.size();
			    k++)
				if(RegionDelta(v.fn->blocks[b].instructions[k]) > 0)
					max_depth++;
	}

	void Walk(size_t block, size_t start, int depth,
	          set<std::pair<size_t, int> > & seen)
	{
		// The initial call scans a block tail (start > 0) and is not
		// memoized: a back edge re-entering that block at the same
		// depth must still scan its leading instructions. Depths past
		// the total push count mean a pushing cycle: irregular shape.
		if(!valid || depth > max_depth)
		{
			valid = false;
			return;
		}
		if(start == 0 &&
		   !seen.insert(std::make_pair(block, depth)).second)
			return;
		const LowIRBlock & at = view.fn->blocks[block];
		for(size_t i = start; i < at.instructions.size(); i++)
		{
			const LowIRInstruction & ins = at.instructions[i];
			if(ins.opcode == LOWIR_INS_EH_END)
			{
				if(depth == 0)
				{
					ends.insert(std::make_pair(block, i));
					return;
				}
				depth--;
			}
			else if(ins.opcode == LOWIR_INS_EH_TRY ||
			        ins.opcode == LOWIR_INS_EH_CLEANUP)
				depth++;
			body.insert(std::make_pair(block, i));
			if(ins.is_terminator())
			{
				// The region must close before leaving the function.
				if(ins.opcode == LOWIR_INS_JUMP ||
				   ins.opcode == LOWIR_INS_BRANCH ||
				   ins.opcode == LOWIR_INS_SWITCH)
				{
					for(size_t t = 0; t < ins.block_targets.size(); t++)
					{
						map<string, size_t>::const_iterator succ =
							view.block_index.find(ins.block_targets[t]);
						if(succ == view.block_index.end())
						{
							valid = false;
							return;
						}
						set<std::pair<size_t, int> > & memo = seen;
						Walk(succ->second, 0, depth, memo);
					}
				}
				else
					valid = false;
				return;
			}
		}
		valid = false;  // fell off a block without a terminator
	}
};

}  // namespace

bool StripNoOpEh(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	LowIRFunction & fn = *view.fn;
	if(fn.metadata.find("unwind") != "no")
		return false;

	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		LowIRBlock & block = fn.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); i++)
		{
			const LowIRInstruction & ins = block.instructions[i];
			if(ins.opcode != LOWIR_INS_EH_TRY)
				continue;
			RegionScan scan(view);
			set<std::pair<size_t, int> > seen;
			scan.Walk(b, i + 1, 0, seen);
			if(!scan.valid || scan.ends.empty())
				continue;
			bool transfers = false;
			for(set<std::pair<size_t, size_t> >::const_iterator at =
				    scan.body.begin();
			    at != scan.body.end(); ++at)
			{
				const LowIRInstruction & inner =
					fn.blocks[at->first].instructions[at->second];
				if(MayTransferToHandler(*view.context, inner))
					transfers = true;
			}
			if(transfers)
				continue;
			// Delete the matching eh_end sites and this eh_try; the
			// handler block becomes unreachable and the cleanup pass
			// removes it.
			vector<std::pair<size_t, size_t> > removals(
				scan.ends.begin(), scan.ends.end());
			removals.push_back(std::make_pair(b, i));
			for(size_t r = 0; r < removals.size(); r++)
			{
				// Erase highest index first within each block.
				size_t best = r;
				for(size_t j = r + 1; j < removals.size(); j++)
					if(removals[j].first > removals[best].first ||
					   (removals[j].first == removals[best].first &&
					    removals[j].second > removals[best].second))
						best = j;
				std::swap(removals[r], removals[best]);
				LowIRBlock & victim = fn.blocks[removals[r].first];
				victim.instructions.erase(
					victim.instructions.begin() +
					static_cast<long>(removals[r].second));
			}
			BuildOptFunction(view, fn, *view.context);
			return true;
		}
	}
	return false;
}
