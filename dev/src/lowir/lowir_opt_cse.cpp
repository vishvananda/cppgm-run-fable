// PA37 pure-expression reuse: executable-edge available-expression
// analysis over addr/index/unary/binary/cmp/convert with commutative
// operand normalization and reversible compare directions. Handler
// blocks only receive expressions available at the region push point.

#include "lowir/lowir_dump.h"
#include "lowir/lowir_opt_internal.h"

namespace {

typedef map<string, string> AvailMap;  // expression key -> temp name

bool CommutativeBinary(const LowIRInstruction & ins)
{
	if(ins.opcode == LOWIR_INS_BINARY)
		return ins.operation == "add" || ins.operation == "mul" ||
			ins.operation == "and" || ins.operation == "or" ||
			ins.operation == "xor";
	if(ins.opcode == LOWIR_INS_CMP)
		return ins.operation == "eq" || ins.operation == "ne";
	return false;
}

string ReversedPredicate(const string & op)
{
	if(op == "gt")
		return "lt";
	if(op == "ge")
		return "le";
	if(op == "ugt")
		return "ult";
	if(op == "uge")
		return "ule";
	return "";
}

// Canonical matching key of a reusable pure expression ("" when the
// instruction is not a reuse candidate).
string ExpressionKey(const LowIRInstruction & ins)
{
	switch(ins.opcode)
	{
	case LOWIR_INS_ADDR:
	case LOWIR_INS_INDEX:
	case LOWIR_INS_UNARY:
	case LOWIR_INS_BINARY:
	case LOWIR_INS_CMP:
	case LOWIR_INS_CONVERT:
		break;
	default:
		return "";
	}
	LowIRInstruction key = ins;
	key.result = "";
	if(key.opcode == LOWIR_INS_CMP)
	{
		string reversed = ReversedPredicate(key.operation);
		if(!reversed.empty())
		{
			key.operation = reversed;
			std::swap(key.operands[0], key.operands[1]);
		}
	}
	if(CommutativeBinary(key))
	{
		string a = LowIROperandText(key.operands[0]);
		string b = LowIROperandText(key.operands[1]);
		if(b < a)
			std::swap(key.operands[0], key.operands[1]);
	}
	return LowIRInstructionText(key);
}

void IntersectInto(AvailMap & into, const AvailMap & from)
{
	AvailMap::iterator it = into.begin();
	while(it != into.end())
	{
		AvailMap::const_iterator theirs = from.find(it->first);
		if(theirs == from.end() || theirs->second != it->second)
			into.erase(it++);
		else
			++it;
	}
}

struct CsePass
{
	OptFunction & view;
	LowIRFunction & fn;

	// TOP (universal availability) is represented by has_out=false.
	vector<AvailMap> out_map;
	vector<char> has_out;
	vector<AvailMap> handler_contrib;
	vector<char> has_handler_contrib;

	explicit CsePass(OptFunction & v) : view(v), fn(*v.fn) {}

	bool ComputeIn(size_t b, AvailMap & in) const;
	void WalkBlock(size_t b, AvailMap state, bool record,
	               OptSubstitution * subst,
	               set<std::pair<size_t, size_t> > * dups);
	bool Run();
};

// in[b] = intersection over normal predecessor outs plus handler
// contributions; false while every input is still TOP.
bool CsePass::ComputeIn(size_t b, AvailMap & in) const
{
	if(b == 0)
	{
		in.clear();
		return true;
	}
	bool have = false;
	for(size_t p = 0; p < fn.blocks.size(); p++)
	{
		if(fn.blocks[p].instructions.empty())
			continue;
		vector<string> targets;
		TerminatorTargets(fn.blocks[p].instructions.back(), targets);
		for(size_t t = 0; t < targets.size(); t++)
		{
			map<string, size_t>::const_iterator succ =
				view.block_index.find(targets[t]);
			if(succ == view.block_index.end() || succ->second != b)
				continue;
			if(!has_out[p])
				continue;
			if(!have)
			{
				in = out_map[p];
				have = true;
			}
			else
				IntersectInto(in, out_map[p]);
		}
	}
	if(has_handler_contrib[b])
	{
		if(!have)
		{
			in = handler_contrib[b];
			have = true;
		}
		else
			IntersectInto(in, handler_contrib[b]);
	}
	return have;
}

void CsePass::WalkBlock(size_t b, AvailMap state, bool record,
                        OptSubstitution * subst,
                        set<std::pair<size_t, size_t> > * dups)
{
	LowIRBlock & block = fn.blocks[b];
	for(size_t k = 0; k < block.instructions.size(); k++)
	{
		LowIRInstruction & ins = block.instructions[k];
		if(record &&
		   (ins.opcode == LOWIR_INS_EH_TRY ||
		    ins.opcode == LOWIR_INS_EH_CLEANUP))
		{
			map<string, size_t>::const_iterator target =
				view.block_index.find(ins.block_targets[0]);
			if(target != view.block_index.end())
			{
				size_t h = target->second;
				if(!has_handler_contrib[h])
				{
					handler_contrib[h] = state;
					has_handler_contrib[h] = 1;
				}
				else
					IntersectInto(handler_contrib[h], state);
			}
		}
		string key = ExpressionKey(ins);
		if(key.empty())
			continue;
		AvailMap::const_iterator known = state.find(key);
		if(known != state.end())
		{
			if(subst && known->second != ins.result)
			{
				LowIROperand replacement;
				replacement.kind = LOWIR_OPERAND_TEMP;
				replacement.name = known->second;
				(*subst)[ins.result] = replacement;
				dups->insert(std::make_pair(b, k));
			}
			continue;
		}
		if(!ins.result.empty())
			state[key] = ins.result;
	}
	if(record)
	{
		out_map[b] = state;
		has_out[b] = 1;
	}
}

bool CsePass::Run()
{
	size_t count = fn.blocks.size();
	out_map.assign(count, AvailMap());
	has_out.assign(count, 0);
	handler_contrib.assign(count, AvailMap());
	has_handler_contrib.assign(count, 0);

	for(int iter = 0; iter < 64; iter++)
	{
		bool changed = false;
		vector<AvailMap> prior_out = out_map;
		vector<char> prior_has = has_out;
		for(size_t b = 0; b < count; b++)
		{
			AvailMap in;
			if(!ComputeIn(b, in) && b != 0)
				continue;
			WalkBlock(b, in, true, nullptr, nullptr);
		}
		for(size_t b = 0; b < count; b++)
			if(has_out[b] != prior_has[b] ||
			   (has_out[b] && out_map[b] != prior_out[b]))
				changed = true;
		if(!changed)
			break;
	}

	// Rewrite duplicates against the stable availability.
	OptSubstitution subst;
	set<std::pair<size_t, size_t> > dups;
	for(size_t b = 0; b < count; b++)
	{
		AvailMap in;
		if(!ComputeIn(b, in) && b != 0)
			continue;
		WalkBlock(b, in, false, &subst, &dups);
	}
	if(subst.empty())
		return false;

	// Lexical discipline: a replacement must be defined before every
	// rewritten use; the duplicate's definition site bounds them.
	OptSubstitution safe;
	for(OptSubstitution::const_iterator it = subst.begin();
	    it != subst.end(); ++it)
	{
		map<string, OptInsRef>::const_iterator dup =
			view.temp_defs.find(it->first);
		map<string, OptInsRef>::const_iterator first =
			view.temp_defs.find(it->second.name);
		if(dup == view.temp_defs.end() || first == view.temp_defs.end())
			continue;
		if(first->second.block < dup->second.block ||
		   (first->second.block == dup->second.block &&
		    first->second.index < dup->second.index))
			safe[it->first] = it->second;
	}
	if(safe.empty())
		return false;
	return ApplySubstitution(fn, safe);
}

}  // namespace

bool ReuseExpressions(OptFunction & view)
{
	if(!view.optimizable)
		return false;
	CsePass pass(view);
	return pass.Run();
}
