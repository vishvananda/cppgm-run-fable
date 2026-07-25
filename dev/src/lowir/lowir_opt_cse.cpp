// PA37 pure-expression reuse: executable-edge available-expression
// analysis over addr/index/unary/binary/cmp/convert with commutative
// operand normalization and reversible compare directions. Handler
// blocks only receive expressions available at the region push point.

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

string OperandKey(const LowIROperand & op)
{
	string out = std::to_string(op.kind);
	out += op.negated ? '-' : '+';
	if(op.kind == LOWIR_OPERAND_LITERAL)
		out += std::to_string(op.literal_class) + ':' + op.literal;
	else
		out += op.name;
	return out;
}

string TypeKey(const LowIRType & type)
{
	string out = std::to_string(type.kind);
	if(type.is_obj())
		out += 'x' + std::to_string(type.obj_bytes) + 'x' +
			std::to_string(type.obj_align);
	return out;
}

// Canonical matching key of a reusable pure expression ("" when the
// instruction is not a reuse candidate). Built from the semantic
// fields directly so identity never depends on the dump presentation.
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
	string operation = ins.operation;
	string a = OperandKey(ins.operands[0]);
	string b = ins.operands.size() > 1 ? OperandKey(ins.operands[1])
	                                   : string();
	if(ins.opcode == LOWIR_INS_CMP)
	{
		string reversed = ReversedPredicate(operation);
		if(!reversed.empty())
		{
			operation = reversed;
			std::swap(a, b);
		}
	}
	if(CommutativeBinary(ins) && b < a)
		std::swap(a, b);
	string key = std::to_string(ins.opcode) + '|' + operation + '|' +
		TypeKey(ins.type) + '|' + TypeKey(ins.type2);
	for(size_t m = 0; m < ins.metadata.items.size(); m++)
		key += '|' + ins.metadata.items[m].first + '=' +
			ins.metadata.items[m].second;
	key += '|' + a + '|' + b;
	return key;
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
	vector<vector<size_t> > preds;   // normal-edge predecessors

	explicit CsePass(OptFunction & v) : view(v), fn(*v.fn) {}

	void BuildPredecessors();
	bool ComputeIn(size_t b, AvailMap & in) const;
	void WalkBlock(size_t b, AvailMap state, bool record,
	               OptSubstitution * subst);
	bool Run();
};

void CsePass::BuildPredecessors()
{
	preds.assign(fn.blocks.size(), vector<size_t>());
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
			if(succ != view.block_index.end())
				preds[succ->second].push_back(p);
		}
	}
}

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
	for(size_t t = 0; t < preds[b].size(); t++)
	{
		size_t p = preds[b][t];
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
                        OptSubstitution * subst)
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
	BuildPredecessors();

	// The descent is monotone from TOP; an unconverged capped run
	// over-approximates availability, so no rewrites happen then.
	bool converged = false;
	for(int iter = 0; iter < 64 && !converged; iter++)
	{
		bool changed = false;
		vector<AvailMap> prior_out = out_map;
		vector<char> prior_has = has_out;
		for(size_t b = 0; b < count; b++)
		{
			AvailMap in;
			if(!ComputeIn(b, in) && b != 0)
				continue;
			WalkBlock(b, in, true, nullptr);
		}
		for(size_t b = 0; b < count; b++)
			if(has_out[b] != prior_has[b] ||
			   (has_out[b] && out_map[b] != prior_out[b]))
				changed = true;
		converged = !changed;
	}
	if(!converged)
		return false;

	// Rewrite duplicates against the stable availability.
	OptSubstitution subst;
	for(size_t b = 0; b < count; b++)
	{
		AvailMap in;
		if(!ComputeIn(b, in) && b != 0)
			continue;
		WalkBlock(b, in, false, &subst);
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
