// PA37 dead-code pass: unused pure temp producers, removable readnone
// calls, dead local-slot traffic, all-or-nothing same-block slot
// store-to-load forwarding (including the inf/nan bit-image fold), and
// unused slot declarations. Level 2 adds dead-store elimination on
// promotable slots via exceptional-edge-aware liveness.

#include "lowir/lowir_opt_internal.h"

namespace {

// Occurrence summary of one slot across the body.
struct SlotUses
{
	vector<OptInsRef> loads;      // LOAD with direct slot storage
	vector<OptInsRef> stores;     // STORE with direct slot storage
	size_t other = 0;             // any other operand occurrence
};

void CollectSlotUses(const LowIRFunction & fn,
                     map<string, SlotUses> & uses)
{
	uses.clear();
	for(size_t i = 0; i < fn.slots.size(); i++)
		uses[fn.slots[i].name];
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			OptInsRef at;
			at.block = b;
			at.index = k;
			for(size_t o = 0; o < ins.operands.size(); o++)
			{
				const LowIROperand & op = ins.operands[o];
				if(op.kind != LOWIR_OPERAND_SLOT)
					continue;
				if(ins.opcode == LOWIR_INS_LOAD && o == 0)
					uses[op.name].loads.push_back(at);
				else if(ins.opcode == LOWIR_INS_STORE && o == 1)
					uses[op.name].stores.push_back(at);
				else
					uses[op.name].other++;
			}
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_SLOT)
					uses[ins.switch_values[o].name].other++;
		}
}

void CountTempUses(const LowIRFunction & fn, map<string, size_t> & uses)
{
	uses.clear();
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			for(size_t o = 0; o < ins.operands.size(); o++)
				if(ins.operands[o].kind == LOWIR_OPERAND_TEMP)
					uses[ins.operands[o].name]++;
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_TEMP)
					uses[ins.switch_values[o].name]++;
			if(ins.opcode == LOWIR_INS_CALL && ins.callee_is_temp)
				uses[ins.callee]++;
		}
}

bool PureRemovable(const LowIRInstruction & ins)
{
	switch(ins.opcode)
	{
	case LOWIR_INS_CONST:
	case LOWIR_INS_COPY:
	case LOWIR_INS_ADDR:
	case LOWIR_INS_INDEX:
	case LOWIR_INS_UNARY:
	case LOWIR_INS_BINARY:
	case LOWIR_INS_CMP:
	case LOWIR_INS_CONVERT:
		return true;
	case LOWIR_INS_LOAD:
		return ins.operands[0].kind == LOWIR_OPERAND_SLOT;
	default:
		return false;
	}
}

// A call is removable when unused only if the callee is spelled
// readnone, cannot unwind, and is not noreturn.
bool RemovableCall(const LowIROptContext & context,
                   const LowIRInstruction & ins)
{
	if(ins.opcode != LOWIR_INS_CALL)
		return false;
	const LowIRMetadata * metadata = nullptr;
	if(ins.callee_is_temp)
		metadata = ins.signature.present ? &ins.signature.metadata
		                                 : nullptr;
	else
	{
		const LowIRFunction * callee = context.FindFunction(ins.callee);
		if(callee)
			metadata = &callee->metadata;
		else if(ins.signature.present)
			metadata = &ins.signature.metadata;
	}
	if(!metadata)
		return false;
	return metadata->find("effects") == "readnone" &&
		metadata->find("unwind") == "no" &&
		metadata->find("return") != "noreturn";
}

struct DcePass
{
	OptFunction & view;
	LowIRFunction & fn;

	explicit DcePass(OptFunction & v) : view(v), fn(*v.fn) {}

	bool RemoveUnusedInstructions();
	bool RemoveDeadSlotStores();
	bool ForwardSameBlockSlots();
	bool RemoveUnusedSlots();
	bool RemovePromotedDeadStores();
	bool EraseMarked(const set<std::pair<size_t, size_t> > & marks);
};

bool DcePass::EraseMarked(const set<std::pair<size_t, size_t> > & marks)
{
	if(marks.empty())
		return false;
	for(set<std::pair<size_t, size_t> >::const_reverse_iterator it =
		    marks.rbegin();
	    it != marks.rend(); ++it)
	{
		vector<LowIRInstruction> & list =
			fn.blocks[it->first].instructions;
		list.erase(list.begin() + static_cast<long>(it->second));
	}
	return true;
}

bool DcePass::RemoveUnusedInstructions()
{
	map<string, size_t> uses;
	CountTempUses(fn, uses);
	set<std::pair<size_t, size_t> > marks;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			if(!ins.result.empty() && uses[ins.result] == 0 &&
			   PureRemovable(ins))
				marks.insert(std::make_pair(b, k));
			else if(ins.opcode == LOWIR_INS_CALL &&
			        (ins.result.empty() || uses[ins.result] == 0) &&
			        RemovableCall(*view.context, ins))
				marks.insert(std::make_pair(b, k));
		}
	return EraseMarked(marks);
}

bool DcePass::RemoveDeadSlotStores()
{
	map<string, SlotUses> slots;
	CollectSlotUses(fn, slots);
	set<std::pair<size_t, size_t> > marks;
	for(map<string, SlotUses>::const_iterator it = slots.begin();
	    it != slots.end(); ++it)
	{
		const SlotUses & uses = it->second;
		if(uses.other != 0 || !uses.loads.empty())
			continue;
		for(size_t s = 0; s < uses.stores.size(); s++)
			marks.insert(std::make_pair(uses.stores[s].block,
			                            uses.stores[s].index));
	}
	return EraseMarked(marks);
}

// The inf/nan bit-image fold: a same-width integer store image read
// back as a float becomes the float literal when the image spells an
// infinity or NaN.
bool PunnedFloatLiteral(const LowIRType & store_type,
                        const LowIRType & load_type,
                        const LowIROperand & value, LowIROperand & out)
{
	unsigned long long bits = 0;
	if(!OperandIntValue(value, bits))
		return false;
	bool negative;
	bool infinite;
	if(store_type.kind == LOWIR_TYPE_I64 &&
	   load_type.kind == LOWIR_TYPE_F64)
	{
		if(((bits >> 52) & 0x7ff) != 0x7ff)
			return false;
		negative = (bits >> 63) != 0;
		infinite = (bits & ((1ull << 52) - 1)) == 0;
	}
	else if((store_type.kind == LOWIR_TYPE_I32 ||
	         store_type.kind == LOWIR_TYPE_U32) &&
	        load_type.kind == LOWIR_TYPE_F32)
	{
		bits &= 0xffffffffull;
		if(((bits >> 23) & 0xff) != 0xff)
			return false;
		negative = (bits >> 31) != 0;
		infinite = (bits & ((1ull << 23) - 1)) == 0;
	}
	else
		return false;
	out.kind = LOWIR_OPERAND_LITERAL;
	out.negated = negative;
	out.literal = infinite ? "inf" : "nan";
	if(load_type.kind == LOWIR_TYPE_F32)
	{
		out.literal_class = LOWIR_LITERAL_F32;
		if(!infinite)
			out.literal = "nanf";
	}
	else
		out.literal_class = LOWIR_LITERAL_F64;
	return true;
}

bool DcePass::ForwardSameBlockSlots()
{
	map<string, SlotUses> slots;
	CollectSlotUses(fn, slots);
	for(map<string, SlotUses>::const_iterator it = slots.begin();
	    it != slots.end(); ++it)
	{
		const SlotUses & uses = it->second;
		if(uses.other != 0 || uses.stores.size() != 1 ||
		   uses.loads.empty())
			continue;
		const OptInsRef & site = uses.stores[0];
		const LowIRInstruction & store =
			fn.blocks[site.block].instructions[site.index];
		const LowIROperand & value = store.operands[0];
		// Only literal images forward here; parameter/temp spill slots
		// stay intact unless inline cleanup (or level-2 promotion)
		// owns them.
		unsigned long long image_bits = 0;
		if(!OperandIntValue(value, image_bits))
			continue;

		// Every load must sit after the store in the same block with
		// no may-write instruction in between.
		bool forwardable = true;
		size_t last_load = site.index;
		for(size_t l = 0; l < uses.loads.size() && forwardable; l++)
		{
			const OptInsRef & at = uses.loads[l];
			if(at.block != site.block || at.index <= site.index)
				forwardable = false;
			else if(at.index > last_load)
				last_load = at.index;
		}
		for(size_t k = site.index + 1;
		    forwardable && k < last_load; k++)
		{
			const LowIRInstruction & between =
				fn.blocks[site.block].instructions[k];
			bool is_forwarded_load =
				between.opcode == LOWIR_INS_LOAD &&
				between.operands[0].kind == LOWIR_OPERAND_SLOT &&
				between.operands[0].name == it->first;
			if(!is_forwarded_load && MayWriteMemory(between))
				forwardable = false;
		}
		if(!forwardable)
			continue;

		OptSubstitution subst;
		bool typed_ok = true;
		for(size_t l = 0; l < uses.loads.size() && typed_ok; l++)
		{
			const LowIRInstruction & load =
				fn.blocks[uses.loads[l].block]
					.instructions[uses.loads[l].index];
			if(load.type.kind == store.type.kind)
				subst[load.result] = value;
			else
			{
				LowIROperand punned;
				if(PunnedFloatLiteral(store.type, load.type, value,
				                      punned))
					subst[load.result] = punned;
				else
					typed_ok = false;
			}
		}
		if(!typed_ok || !ApplySubstitution(fn, subst))
			continue;

		set<std::pair<size_t, size_t> > marks;
		marks.insert(std::make_pair(site.block, site.index));
		for(size_t l = 0; l < uses.loads.size(); l++)
			marks.insert(std::make_pair(uses.loads[l].block,
			                            uses.loads[l].index));
		EraseMarked(marks);
		return true;
	}
	return false;
}

bool DcePass::RemoveUnusedSlots()
{
	map<string, SlotUses> slots;
	CollectSlotUses(fn, slots);
	vector<LowIRSlot> kept;
	for(size_t i = 0; i < fn.slots.size(); i++)
	{
		const SlotUses & uses = slots[fn.slots[i].name];
		if(uses.other != 0 || !uses.loads.empty() ||
		   !uses.stores.empty())
			kept.push_back(fn.slots[i]);
	}
	if(kept.size() == fn.slots.size())
		return false;
	fn.slots.swap(kept);
	return true;
}

// Level-2 dead stores on promotable slots: a store no observable load
// (including via an exceptional edge) can see is removed.
bool DcePass::RemovePromotedDeadStores()
{
	if(view.context->level < 2)
		return false;
	map<string, SlotUses> slots;
	CollectSlotUses(fn, slots);
	set<std::pair<size_t, size_t> > marks;
	for(map<string, SlotUses>::const_iterator it = slots.begin();
	    it != slots.end(); ++it)
	{
		const SlotUses & uses = it->second;
		if(uses.other != 0 || uses.stores.empty())
			continue;
		const string & slot = it->first;

		// Block-level liveness of this slot over normal edges plus a
		// conservative all-handlers exceptional edge.
		size_t count = fn.blocks.size();
		vector<int> first_access(count, 0);  // 0 none, 1 load, 2 store
		for(size_t b = 0; b < count; b++)
			for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			{
				const LowIRInstruction & ins =
					fn.blocks[b].instructions[k];
				if(ins.opcode == LOWIR_INS_LOAD &&
				   ins.operands[0].kind == LOWIR_OPERAND_SLOT &&
				   ins.operands[0].name == slot)
				{
					first_access[b] = 1;
					break;
				}
				if(ins.opcode == LOWIR_INS_STORE &&
				   ins.operands[1].kind == LOWIR_OPERAND_SLOT &&
				   ins.operands[1].name == slot)
				{
					first_access[b] = 2;
					break;
				}
			}
		vector<bool> live_in(count, false), live_out(count, false);
		bool stable = false;
		while(!stable)
		{
			stable = true;
			for(size_t b = count; b-- > 0;)
			{
				bool out = false;
				if(!fn.blocks[b].instructions.empty())
				{
					vector<string> targets;
					TerminatorTargets(fn.blocks[b].instructions.back(),
					                  targets);
					for(size_t t = 0; t < targets.size(); t++)
					{
						map<string, size_t>::const_iterator succ =
							view.block_index.find(targets[t]);
						if(succ != view.block_index.end())
							out = out || live_in[succ->second];
					}
				}
				bool in = first_access[b] == 1 ||
					(first_access[b] == 0 && out);
				if(out != live_out[b] || in != live_in[b])
				{
					live_out[b] = out;
					live_in[b] = in;
					stable = false;
				}
			}
		}

		// Exceptional observation: liveness into any handler target.
		bool handler_live = false;
		for(size_t b = 0; b < count; b++)
			if(view.handler_target[b] && live_in[b])
				handler_live = true;

		for(size_t b = 0; b < count; b++)
		{
			bool live = live_out[b];
			for(size_t k = fn.blocks[b].instructions.size(); k-- > 0;)
			{
				const LowIRInstruction & ins =
					fn.blocks[b].instructions[k];
				if(ins.opcode == LOWIR_INS_LOAD &&
				   ins.operands[0].kind == LOWIR_OPERAND_SLOT &&
				   ins.operands[0].name == slot)
					live = true;
				else if(ins.opcode == LOWIR_INS_STORE &&
				        ins.operands[1].kind == LOWIR_OPERAND_SLOT &&
				        ins.operands[1].name == slot)
				{
					if(!live)
						marks.insert(std::make_pair(b, k));
					live = false;
				}
				else if(handler_live &&
				        MayTransferToHandler(*view.context, ins) &&
				        view.depth_in[b] >= 0)
					live = true;
			}
		}
	}
	return EraseMarked(marks);
}

}  // namespace

bool ApplySubstitution(LowIRFunction & fn, const OptSubstitution & subst)
{
	if(subst.empty())
		return true;
	// Legality scan: literals cannot land in storage or callee slots.
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			if(ins.opcode == LOWIR_INS_CALL && ins.callee_is_temp)
			{
				OptSubstitution::const_iterator found =
					subst.find(ins.callee);
				if(found != subst.end() &&
				   found->second.kind != LOWIR_OPERAND_TEMP)
					return false;
			}
			for(size_t o = 0; o < ins.operands.size(); o++)
			{
				const LowIROperand & op = ins.operands[o];
				if(op.kind != LOWIR_OPERAND_TEMP)
					continue;
				bool storage_position =
					(ins.opcode == LOWIR_INS_LOAD && o == 0) ||
					(ins.opcode == LOWIR_INS_STORE && o == 1);
				if(!storage_position)
					continue;
				OptSubstitution::const_iterator found =
					subst.find(op.name);
				if(found != subst.end() &&
				   found->second.kind == LOWIR_OPERAND_LITERAL)
					return false;
			}
		}
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			LowIRInstruction & ins = fn.blocks[b].instructions[k];
			if(ins.opcode == LOWIR_INS_CALL && ins.callee_is_temp)
			{
				OptSubstitution::const_iterator found =
					subst.find(ins.callee);
				if(found != subst.end())
					ins.callee = found->second.name;
			}
			for(int list = 0; list < 2; list++)
			{
				vector<LowIROperand> & operands =
					list ? ins.switch_values : ins.operands;
				for(size_t o = 0; o < operands.size(); o++)
				{
					if(operands[o].kind != LOWIR_OPERAND_TEMP)
						continue;
					OptSubstitution::const_iterator found =
						subst.find(operands[o].name);
					if(found != subst.end())
						operands[o] = found->second;
				}
			}
		}
	return true;
}

bool EliminateDeadCode(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	bool changed = false;
	for(;;)
	{
		DcePass pass(view);
		bool round = false;
		round |= pass.RemoveUnusedInstructions();
		round |= pass.RemoveDeadSlotStores();
		round |= pass.ForwardSameBlockSlots();
		round |= pass.RemovePromotedDeadStores();
		round |= pass.RemoveUnusedSlots();
		if(!round)
			break;
		changed = true;
		BuildOptFunction(view, *view.fn, *view.context);
	}
	return changed;
}
