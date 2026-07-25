// PA37 conservative inliner for small direct calls. Naming contract:
// each successful site takes the caller's next counter N and prefixes
// every pasted temp, label, and slot with __o1inl<N>__. Callee
// parameter-spill slots forward to the argument values. Multi-return
// callees merge through a $__o1inl<N>__retmerge__1 (scalar) or
// $__o1inl<N>__retmergeobj__1 (object) slot. Calls inside an active
// EH region only inline callees that cannot transfer to a handler;
// calls in handler code never inline.

#include "lowir/lowir_opt_internal.h"

namespace {

const size_t kPlainInlineBar = 24;
const size_t kPreferLocalInlineBar = 64;

size_t BodySize(const LowIRFunction & fn)
{
	size_t count = 0;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		count += fn.blocks[b].instructions.size();
	return count;
}

bool HasEhInstruction(const LowIRFunction & fn)
{
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			switch(fn.blocks[b].instructions[k].opcode)
			{
			case LOWIR_INS_EH_TRY:
			case LOWIR_INS_EH_CLEANUP:
			case LOWIR_INS_EH_END:
			case LOWIR_INS_EH_MARKER:
			case LOWIR_INS_EXCEPTION:
				return true;
			default:
				break;
			}
	return false;
}

bool BodyMayTransfer(const LowIROptContext & context,
                     const LowIRFunction & fn)
{
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			if(MayTransferToHandler(context,
			                        fn.blocks[b].instructions[k]))
				return true;
	return false;
}

// Region depth right before instruction (b, k).
int DepthAt(const OptFunction & view, size_t b, size_t k)
{
	if(view.depth_in[b] < 0)
		return -1;
	int depth = view.depth_in[b];
	for(size_t i = 0; i < k; i++)
	{
		const LowIRInstruction & ins =
			view.fn->blocks[b].instructions[i];
		if(ins.opcode == LOWIR_INS_EH_TRY ||
		   ins.opcode == LOWIR_INS_EH_CLEANUP)
			depth++;
		else if(ins.opcode == LOWIR_INS_EH_END && depth > 0)
			depth--;
	}
	return depth;
}

struct ReturnSite
{
	size_t block = 0;
	LowIROperand value;   // scalar/ptr value or obj slot operand
	bool has_value = false;
};

struct InlinePlan
{
	OptInsRef site;
	const LowIRFunction * callee = nullptr;
	string prefix;
	map<string, LowIROperand> param_args;  // param temp -> argument
	set<string> forwarded_slots;           // param-spill slots dropped
	map<string, LowIROperand> slot_loads;  // spill loads -> argument
	vector<ReturnSite> returns;
	bool object_return = false;
};

struct Inliner
{
	OptFunction & view;
	LowIRFunction & fn;
	LowIROptContext & context;

	Inliner(OptFunction & v)
		: view(v), fn(*v.fn), context(*v.context)
	{
	}

	bool NameTaken(const string & prefix) const;
	bool CalleeEligible(const LowIRInstruction & call, size_t b,
	                    size_t k, const LowIRFunction *& callee) const;
	bool PlanParamSpills(InlinePlan & plan) const;
	bool PlanReturns(InlinePlan & plan,
	                 const map<string, size_t> & result_uses) const;
	bool RenameOperand(const InlinePlan & plan,
	                   LowIROperand & op) const;
	bool RenameInstruction(const InlinePlan & plan,
	                       LowIRInstruction & ins) const;
	bool Expand(const InlinePlan & plan);
	bool RunOnce();
};

bool Inliner::NameTaken(const string & prefix) const
{
	for(size_t i = 0; i < fn.slots.size(); i++)
		if(fn.slots[i].name.compare(0, prefix.size(), prefix) == 0)
			return true;
	for(size_t b = 0; b < fn.blocks.size(); b++)
	{
		if(fn.blocks[b].label.compare(0, prefix.size(), prefix) == 0)
			return true;
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			if(!ins.result.empty() &&
			   ins.result.compare(0, prefix.size(), prefix) == 0)
				return true;
		}
	}
	return false;
}

bool Inliner::CalleeEligible(const LowIRInstruction & call, size_t b,
                             size_t k,
                             const LowIRFunction *& callee) const
{
	if(call.opcode != LOWIR_INS_CALL || call.callee_is_temp)
		return false;
	callee = context.FindFunction(call.callee);
	if(!callee || !callee->is_definition || callee == &fn)
		return false;
	if(context.inline_cycle.count(callee->name))
		return false;
	if(callee->metadata.find("arity") == "variadic" ||
	   call.signature.present)
		return false;
	if(callee->params.size() != call.operands.size())
		return false;
	size_t bar = callee->metadata.find("prefer_local") == "yes"
		? kPreferLocalInlineBar : kPlainInlineBar;
	if(BodySize(*callee) > bar)
		return false;
	if(view.handler_context[b])
		return false;
	int depth = DepthAt(view, b, k);
	if(depth < 0)
		return false;
	if(depth > 0)
	{
		// Inside a protected region: the pasted body must not be able
		// to transfer to the region's handler.
		if(HasEhInstruction(*callee))
			return false;
		if(callee->metadata.find("unwind") != "no" &&
		   BodyMayTransfer(context, *callee))
			return false;
	}
	return true;
}

// Leading entry-block parameter spills (`store T %param, $slot`) whose
// slot is written once and never escapes forward to the arguments.
bool Inliner::PlanParamSpills(InlinePlan & plan) const
{
	const LowIRFunction & callee = *plan.callee;
	map<string, size_t> store_counts;
	set<string> hard_slots;   // any non load/store-direct occurrence
	map<string, vector<const LowIRInstruction *> > loads;
	for(size_t b = 0; b < callee.blocks.size(); b++)
		for(size_t k = 0; k < callee.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins =
				callee.blocks[b].instructions[k];
			for(size_t o = 0; o < ins.operands.size(); o++)
			{
				const LowIROperand & op = ins.operands[o];
				if(op.kind != LOWIR_OPERAND_SLOT)
					continue;
				if(ins.opcode == LOWIR_INS_LOAD && o == 0)
					loads[op.name].push_back(&ins);
				else if(ins.opcode == LOWIR_INS_STORE && o == 1)
					store_counts[op.name]++;
				else
					hard_slots.insert(op.name);
			}
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_SLOT)
					hard_slots.insert(ins.switch_values[o].name);
		}

	const LowIRBlock & entry = callee.blocks[0];
	for(size_t k = 0; k < entry.instructions.size(); k++)
	{
		const LowIRInstruction & ins = entry.instructions[k];
		if(ins.opcode != LOWIR_INS_STORE ||
		   ins.operands[0].kind != LOWIR_OPERAND_TEMP ||
		   ins.operands[1].kind != LOWIR_OPERAND_SLOT)
			break;
		map<string, LowIROperand>::const_iterator param =
			plan.param_args.find(ins.operands[0].name);
		if(param == plan.param_args.end())
			break;
		const string & slot = ins.operands[1].name;
		if(store_counts[slot] != 1 || hard_slots.count(slot))
			continue;
		plan.forwarded_slots.insert(slot);
		const vector<const LowIRInstruction *> & slot_loads =
			loads[slot];
		for(size_t l = 0; l < slot_loads.size(); l++)
			plan.slot_loads[slot_loads[l]->result] = param->second;
	}
	return true;
}

bool Inliner::PlanReturns(InlinePlan & plan,
                          const map<string, size_t> & result_uses) const
{
	const LowIRFunction & callee = *plan.callee;
	bool object = callee.return_type.is_obj();
	plan.object_return = object;
	for(size_t b = 0; b < callee.blocks.size(); b++)
	{
		if(callee.blocks[b].instructions.empty())
			return false;
		const LowIRInstruction & term =
			callee.blocks[b].instructions.back();
		if(term.opcode != LOWIR_INS_RETURN)
			continue;
		ReturnSite site;
		site.block = b;
		site.has_value = !term.operands.empty();
		if(site.has_value)
			site.value = term.operands[0];
		if(object &&
		   (!site.has_value ||
		    site.value.kind != LOWIR_OPERAND_SLOT))
			return false;
		plan.returns.push_back(site);
	}
	if(plan.returns.empty())
		return false;

	// The consumed object result must feed copyobj sources only.
	const LowIRInstruction & call =
		fn.blocks[plan.site.block].instructions[plan.site.index];
	if(object && !call.result.empty())
	{
		map<string, size_t>::const_iterator uses =
			result_uses.find(call.result);
		size_t remaining = uses == result_uses.end() ? 0 : uses->second;
		for(size_t b = 0; b < fn.blocks.size(); b++)
			for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			{
				const LowIRInstruction & ins =
					fn.blocks[b].instructions[k];
				if(ins.opcode != LOWIR_INS_COPYOBJ)
					continue;
				if(ins.operands[0].kind == LOWIR_OPERAND_TEMP &&
				   ins.operands[0].name == call.result)
					remaining--;
			}
		if(remaining != 0)
			return false;
	}
	return true;
}

bool Inliner::RenameOperand(const InlinePlan & plan,
                            LowIROperand & op) const
{
	if(op.kind == LOWIR_OPERAND_TEMP)
	{
		map<string, LowIROperand>::const_iterator arg =
			plan.param_args.find(op.name);
		if(arg != plan.param_args.end())
		{
			op = arg->second;
			return true;
		}
		map<string, LowIROperand>::const_iterator fwd =
			plan.slot_loads.find(op.name);
		if(fwd != plan.slot_loads.end())
		{
			op = fwd->second;
			return true;
		}
		op.name = plan.prefix + op.name;
		return true;
	}
	if(op.kind == LOWIR_OPERAND_SLOT)
	{
		if(plan.forwarded_slots.count(op.name))
			return false;
		op.name = plan.prefix + op.name;
	}
	return true;
}

bool Inliner::RenameInstruction(const InlinePlan & plan,
                                LowIRInstruction & ins) const
{
	if(!ins.result.empty())
	{
		if(plan.slot_loads.count(ins.result))
			return true;  // forwarded spill load: dropped by caller
		ins.result = plan.prefix + ins.result;
	}
	for(size_t o = 0; o < ins.operands.size(); o++)
		if(!RenameOperand(plan, ins.operands[o]))
			return false;
	for(size_t o = 0; o < ins.switch_values.size(); o++)
		if(!RenameOperand(plan, ins.switch_values[o]))
			return false;
	for(size_t t = 0; t < ins.block_targets.size(); t++)
		ins.block_targets[t] = plan.prefix + ins.block_targets[t];
	if(ins.opcode == LOWIR_INS_CALL && ins.callee_is_temp)
	{
		const LowIROperand * replacement = nullptr;
		map<string, LowIROperand>::const_iterator arg =
			plan.param_args.find(ins.callee);
		if(arg != plan.param_args.end())
			replacement = &arg->second;
		else
		{
			map<string, LowIROperand>::const_iterator fwd =
				plan.slot_loads.find(ins.callee);
			if(fwd != plan.slot_loads.end())
				replacement = &fwd->second;
		}
		if(replacement)
		{
			if(replacement->kind != LOWIR_OPERAND_TEMP)
				return false;
			ins.callee = replacement->name;
		}
		else
			ins.callee = plan.prefix + ins.callee;
	}
	return true;
}

bool Inliner::Expand(const InlinePlan & plan)
{
	const LowIRFunction & callee = *plan.callee;
	LowIRBlock & call_block = fn.blocks[plan.site.block];
	LowIRInstruction call = call_block.instructions[plan.site.index];
	bool multi_block = callee.blocks.size() > 1;
	bool multi_return = plan.returns.size() > 1;
	const string cont_label = plan.prefix + "cont";

	// Copy and rename the callee body.
	vector<LowIRBlock> body = callee.blocks;
	vector<size_t> drops;  // forwarded spill instructions in entry
	for(size_t b = 0; b < body.size(); b++)
	{
		body[b].label = plan.prefix + body[b].label;
		vector<LowIRInstruction> kept;
		for(size_t k = 0; k < body[b].instructions.size(); k++)
		{
			LowIRInstruction & ins = body[b].instructions[k];
			bool spill_store = ins.opcode == LOWIR_INS_STORE &&
				ins.operands[1].kind == LOWIR_OPERAND_SLOT &&
				plan.forwarded_slots.count(ins.operands[1].name);
			bool spill_load = ins.opcode == LOWIR_INS_LOAD &&
				!ins.result.empty() &&
				plan.slot_loads.count(ins.result);
			if(spill_store || spill_load)
				continue;
			if(!RenameInstruction(plan, ins))
				return false;
			kept.push_back(ins);
		}
		body[b].instructions.swap(kept);
	}
	(void)drops;

	// Merge slot and result plumbing.
	string merge_slot;
	OptSubstitution result_subst;
	if(multi_return)
		merge_slot = plan.prefix +
			(plan.object_return ? "retmergeobj__1" : "retmerge__1");
	vector<LowIROperand> renamed_returns(plan.returns.size());
	for(size_t r = 0; r < plan.returns.size(); r++)
	{
		LowIRBlock & at = body[plan.returns[r].block];
		LowIRInstruction ret = at.instructions.back();
		at.instructions.pop_back();
		if(!ret.operands.empty())
			renamed_returns[r] = ret.operands[0];
		if(multi_return && !callee.return_type.is_void())
		{
			LowIRInstruction moved;
			if(plan.object_return)
			{
				moved.opcode = LOWIR_INS_COPYOBJ;
				moved.span_bytes = callee.return_type.obj_bytes;
				moved.span_align = callee.return_type.obj_align;
				moved.operands.push_back(ret.operands[0]);
				LowIROperand dest;
				dest.kind = LOWIR_OPERAND_SLOT;
				dest.name = merge_slot;
				moved.operands.push_back(dest);
			}
			else
			{
				moved.opcode = LOWIR_INS_STORE;
				moved.type = callee.return_type;
				moved.operands.push_back(ret.operands[0]);
				LowIROperand dest;
				dest.kind = LOWIR_OPERAND_SLOT;
				dest.name = merge_slot;
				moved.operands.push_back(dest);
			}
			at.instructions.push_back(moved);
		}
		if(multi_block)
		{
			LowIRInstruction jump;
			jump.opcode = LOWIR_INS_JUMP;
			jump.block_targets.push_back(cont_label);
			at.instructions.push_back(jump);
		}
	}
	if(!call.result.empty() && !multi_return)
		result_subst[call.result] = renamed_returns[0];

	// Append the callee's surviving slots, then the merge slot.
	for(size_t s = 0; s < callee.slots.size(); s++)
	{
		if(plan.forwarded_slots.count(callee.slots[s].name))
			continue;
		LowIRSlot slot = callee.slots[s];
		slot.name = plan.prefix + slot.name;
		fn.slots.push_back(slot);
	}
	if(multi_return)
	{
		LowIRSlot slot;
		slot.name = merge_slot;
		slot.type = callee.return_type;
		fn.slots.push_back(slot);
	}

	// Splice into the caller.
	vector<LowIRInstruction> head(
		call_block.instructions.begin(),
		call_block.instructions.begin() +
			static_cast<long>(plan.site.index));
	vector<LowIRInstruction> tail(
		call_block.instructions.begin() +
			static_cast<long>(plan.site.index) + 1,
		call_block.instructions.end());

	if(!multi_block)
	{
		// Single-block callee: paste inline into the call block.
		head.insert(head.end(), body[0].instructions.begin(),
		            body[0].instructions.end());
		if(multi_return)
		{
			// Single block implies a single terminator/return.
			return false;
		}
		head.insert(head.end(), tail.begin(), tail.end());
		call_block.instructions.swap(head);
	}
	else
	{
		LowIRInstruction jump;
		jump.opcode = LOWIR_INS_JUMP;
		jump.block_targets.push_back(plan.prefix +
		                             callee.blocks[0].label);
		head.push_back(jump);
		call_block.instructions.swap(head);

		LowIRBlock cont;
		cont.label = cont_label;
		if(multi_return && !callee.return_type.is_void() &&
		   !plan.object_return && !call.result.empty())
		{
			LowIRInstruction load;
			load.opcode = LOWIR_INS_LOAD;
			load.type = callee.return_type;
			load.result = call.result;
			LowIROperand src;
			src.kind = LOWIR_OPERAND_SLOT;
			src.name = merge_slot;
			load.operands.push_back(src);
			cont.instructions.push_back(load);
		}
		cont.instructions.insert(cont.instructions.end(), tail.begin(),
		                         tail.end());
		body.push_back(cont);
		fn.blocks.insert(fn.blocks.begin() +
		                 static_cast<long>(plan.site.block) + 1,
		                 body.begin(), body.end());
	}

	if(multi_return && plan.object_return && !call.result.empty())
	{
		LowIROperand merged;
		merged.kind = LOWIR_OPERAND_SLOT;
		merged.name = merge_slot;
		result_subst[call.result] = merged;
	}
	if(!result_subst.empty() && !ApplySubstitution(fn, result_subst))
		return false;
	return true;
}

bool Inliner::RunOnce()
{
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & call = fn.blocks[b].instructions[k];
			const LowIRFunction * callee = nullptr;
			if(!CalleeEligible(call, b, k, callee))
				continue;

			InlinePlan plan;
			plan.site.block = b;
			plan.site.index = k;
			plan.callee = callee;
			for(size_t p = 0; p < callee->params.size(); p++)
				plan.param_args[callee->params[p].name] =
					call.operands[p];
			if(!PlanParamSpills(plan))
				continue;
			map<string, size_t> result_uses;
			{
				// Use counts of the call result for the object gate.
				map<string, size_t> uses;
				for(size_t bb = 0; bb < fn.blocks.size(); bb++)
					for(size_t kk = 0;
					    kk < fn.blocks[bb].instructions.size(); kk++)
					{
						const LowIRInstruction & ins =
							fn.blocks[bb].instructions[kk];
						for(size_t o = 0; o < ins.operands.size(); o++)
							if(ins.operands[o].kind ==
							   LOWIR_OPERAND_TEMP)
								uses[ins.operands[o].name]++;
						for(size_t o = 0;
						    o < ins.switch_values.size(); o++)
							if(ins.switch_values[o].kind ==
							   LOWIR_OPERAND_TEMP)
								uses[ins.switch_values[o].name]++;
						if(ins.opcode == LOWIR_INS_CALL &&
						   ins.callee_is_temp)
							uses[ins.callee]++;
					}
				result_uses = uses;
			}
			if(!PlanReturns(plan, result_uses))
				continue;

			size_t & counter =
				view.context->inline_counters[fn.name];
			plan.prefix = "__o1inl" + std::to_string(counter) + "__";
			while(NameTaken(plan.prefix))
				plan.prefix = "__o1inl" +
					std::to_string(++counter) + "__";

			LowIRFunction saved = fn;
			if(!Expand(plan))
			{
				fn = saved;
				continue;
			}
			counter++;
			return true;
		}
	return false;
}

}  // namespace

namespace {

// Post-inline caller cleanup: the caller's own leading parameter-spill
// slots forward to the parameters, mirroring what the paste does for
// the callee's spills. This runs only for functions that inlined a
// call; standalone bodies keep their spill slots at level 1.
bool ForwardCallerParamSpills(OptFunction & view)
{
	LowIRFunction & fn = *view.fn;
	if(fn.blocks.empty())
		return false;
	map<string, size_t> store_counts;
	set<string> hard_slots;
	map<string, vector<OptInsRef> > loads;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			for(size_t o = 0; o < ins.operands.size(); o++)
			{
				const LowIROperand & op = ins.operands[o];
				if(op.kind != LOWIR_OPERAND_SLOT)
					continue;
				OptInsRef at;
				at.block = b;
				at.index = k;
				if(ins.opcode == LOWIR_INS_LOAD && o == 0)
					loads[op.name].push_back(at);
				else if(ins.opcode == LOWIR_INS_STORE && o == 1)
					store_counts[op.name]++;
				else
					hard_slots.insert(op.name);
			}
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_SLOT)
					hard_slots.insert(ins.switch_values[o].name);
		}

	OptSubstitution subst;
	set<string> dropped;
	set<std::pair<size_t, size_t> > marks;
	const LowIRBlock & entry = fn.blocks[0];
	for(size_t k = 0; k < entry.instructions.size(); k++)
	{
		const LowIRInstruction & ins = entry.instructions[k];
		if(ins.opcode != LOWIR_INS_STORE ||
		   ins.operands[0].kind != LOWIR_OPERAND_TEMP ||
		   ins.operands[1].kind != LOWIR_OPERAND_SLOT ||
		   !view.param_names.count(ins.operands[0].name))
			break;
		const string & slot = ins.operands[1].name;
		if(store_counts[slot] != 1 || hard_slots.count(slot))
			continue;
		dropped.insert(slot);
		marks.insert(std::make_pair((size_t)0, k));
		const vector<OptInsRef> & slot_loads = loads[slot];
		for(size_t l = 0; l < slot_loads.size(); l++)
		{
			const LowIRInstruction & load =
				fn.blocks[slot_loads[l].block]
					.instructions[slot_loads[l].index];
			subst[load.result] = ins.operands[0];
			marks.insert(std::make_pair(slot_loads[l].block,
			                            slot_loads[l].index));
		}
	}
	if(dropped.empty())
		return false;
	if(!ApplySubstitution(fn, subst))
		return false;
	for(set<std::pair<size_t, size_t> >::const_reverse_iterator it =
		    marks.rbegin();
	    it != marks.rend(); ++it)
	{
		vector<LowIRInstruction> & list =
			fn.blocks[it->first].instructions;
		list.erase(list.begin() + static_cast<long>(it->second));
	}
	vector<LowIRSlot> kept;
	for(size_t s = 0; s < fn.slots.size(); s++)
		if(!dropped.count(fn.slots[s].name))
			kept.push_back(fn.slots[s]);
	fn.slots.swap(kept);
	return true;
}

}  // namespace

bool InlineCalls(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	bool any = false;
	for(;;)
	{
		BuildOptFunction(view, *view.fn, *view.context);
		if(!view.optimizable)
			return any;
		Inliner inliner(view);
		if(!inliner.RunOnce())
			break;
		any = true;
	}
	if(any)
	{
		BuildOptFunction(view, *view.fn, *view.context);
		if(view.optimizable && ForwardCallerParamSpills(view))
			BuildOptFunction(view, *view.fn, *view.context);
	}
	return any;
}
