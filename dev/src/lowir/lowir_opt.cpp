// PA37 optimizer driver and the shared per-function view. The pass
// implementations live in lowir_opt_fold.cpp (value lattice),
// lowir_opt_cse.cpp (expression reuse), lowir_opt_cfg.cpp (control
// flow + EH strip), lowir_opt_dce.cpp (dead code and slot traffic),
// and lowir_opt_inline.cpp.

#include "lowir/lowir_opt.h"

#include <cstdlib>

#include "lowir/lowir_opt_internal.h"

void LowIROptContext::RefreshFunctionIndex()
{
	functions_by_name.clear();
	for(size_t i = 0; i < program->functions.size(); i++)
		functions_by_name[program->functions[i].name] =
			&program->functions[i];
}

const LowIRFunction * LowIROptContext::FindFunction(
	const string & name) const
{
	map<string, const LowIRFunction *>::const_iterator found =
		functions_by_name.find(name);
	return found == functions_by_name.end() ? nullptr : found->second;
}

// Call-graph cycle detection over the current bodies: any function on
// a directed cycle of direct calls (including self recursion) is never
// an inline candidate.
void LowIROptContext::RefreshInlineCycles()
{
	map<string, set<string> > callees;
	for(size_t i = 0; i < program->functions.size(); i++)
	{
		const LowIRFunction & fn = program->functions[i];
		if(!fn.is_definition)
			continue;
		set<string> & out = callees[fn.name];
		for(size_t b = 0; b < fn.blocks.size(); b++)
			for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			{
				const LowIRInstruction & ins =
					fn.blocks[b].instructions[k];
				if(ins.opcode == LOWIR_INS_CALL && !ins.callee_is_temp)
					out.insert(ins.callee);
			}
	}

	inline_cycle.clear();
	for(map<string, set<string> >::const_iterator it = callees.begin();
	    it != callees.end(); ++it)
	{
		// Reachability from each callee of `f` back to `f`.
		const string & from = it->first;
		vector<string> work(it->second.begin(), it->second.end());
		set<string> seen(it->second.begin(), it->second.end());
		bool cyclic = seen.count(from) != 0;
		while(!cyclic && !work.empty())
		{
			string at = work.back();
			work.pop_back();
			map<string, set<string> >::const_iterator next =
				callees.find(at);
			if(next == callees.end())
				continue;
			for(set<string>::const_iterator c = next->second.begin();
			    c != next->second.end(); ++c)
			{
				if(*c == from)
				{
					cyclic = true;
					break;
				}
				if(seen.insert(*c).second)
					work.push_back(*c);
			}
		}
		if(cyclic)
			inline_cycle.insert(from);
	}
}

void TerminatorTargets(const LowIRInstruction & ins,
                       vector<string> & targets)
{
	targets.clear();
	if(ins.opcode == LOWIR_INS_JUMP || ins.opcode == LOWIR_INS_BRANCH ||
	   ins.opcode == LOWIR_INS_SWITCH)
		targets = ins.block_targets;
}

bool MayWriteMemory(const LowIRInstruction & ins)
{
	switch(ins.opcode)
	{
	case LOWIR_INS_STORE:
	case LOWIR_INS_ATOMIC_STORE:
	case LOWIR_INS_ATOMIC_EXCHANGE:
	case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
	case LOWIR_INS_ATOMIC_ADD_FETCH:
	case LOWIR_INS_COPYOBJ:
	case LOWIR_INS_ZEROINIT:
	case LOWIR_INS_CALL:
	case LOWIR_INS_THROW:
	case LOWIR_INS_EXCEPTION:
	case LOWIR_INS_EH_TRY:
	case LOWIR_INS_EH_CLEANUP:
	case LOWIR_INS_EH_END:
	case LOWIR_INS_EH_MARKER:
	case LOWIR_INS_RESUME:
		return true;
	default:
		return false;
	}
}

bool CallMayUnwind(const LowIROptContext & context,
                   const LowIRInstruction & ins)
{
	if(ins.opcode != LOWIR_INS_CALL)
		return false;
	if(ins.callee_is_temp)
		return ins.signature.metadata.find("unwind") != "no";
	const LowIRFunction * callee = context.FindFunction(ins.callee);
	if(callee && callee->metadata.find("unwind") == "no")
		return false;
	if(ins.signature.present &&
	   ins.signature.metadata.find("unwind") == "no")
		return false;
	return true;
}

bool MayTransferToHandler(const LowIROptContext & context,
                          const LowIRInstruction & ins)
{
	if(ins.opcode == LOWIR_INS_THROW || ins.opcode == LOWIR_INS_RESUME)
		return true;
	return CallMayUnwind(context, ins);
}

bool SameOperand(const LowIROperand & a, const LowIROperand & b)
{
	return a.kind == b.kind && a.name == b.name && a.literal == b.literal &&
		a.negated == b.negated && a.literal_class == b.literal_class;
}

void BuildOptFunction(OptFunction & view, LowIRFunction & fn,
                      LowIROptContext & context)
{
	view.fn = &fn;
	view.context = &context;
	view.optimizable = fn.is_definition && !fn.blocks.empty();
	view.block_index.clear();
	view.temp_defs.clear();
	view.param_names.clear();

	for(size_t i = 0; i < fn.params.size(); i++)
		view.param_names.insert(fn.params[i].name);
	for(size_t b = 0; b < fn.blocks.size(); b++)
		if(!view.block_index.insert(
			   std::make_pair(fn.blocks[b].label, b)).second)
			view.optimizable = false;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t i = 0; i < fn.blocks[b].instructions.size(); i++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[i];
			if(ins.result.empty())
				continue;
			if(view.param_names.count(ins.result))
				view.optimizable = false;
			OptInsRef ref;
			ref.block = b;
			ref.index = i;
			if(!view.temp_defs.insert(
				   std::make_pair(ins.result, ref)).second)
				view.optimizable = false;
		}
	ComputeEhShape(view);
}

namespace {

bool OptimizeFunctionOnce(OptFunction & view)
{
	bool changed = false;
	for(;;)
	{
		bool round = false;
		round |= FoldAndPropagate(view);
		round |= ReuseExpressions(view);
		round |= CleanupControlFlow(view);
		round |= EliminateDeadCode(view);
		round |= StripNoOpEh(view);
		if(!round)
			break;
		changed = true;
	}
	return changed;
}

bool OptimizeFunction(LowIRFunction & fn, LowIROptContext & context)
{
	OptFunction view;
	BuildOptFunction(view, fn, context);
	if(!view.optimizable)
		return false;
	bool changed = OptimizeFunctionOnce(view);
	while(InlineCalls(view))
	{
		changed = true;
		OptimizeFunctionOnce(view);
	}
	return changed;
}

}  // namespace

void OptimizeLowIRProgram(LowIRProgram & program, int level)
{
	if(level <= 0)
		return;
	LowIROptContext context;
	context.program = &program;
	context.level = level;

	// Program rounds in presentation order: inlining reads the current
	// state of callee bodies, so callees defined later in the file can
	// require a second round of their callers.
	for(int round = 0; round < 32; round++)
	{
		context.RefreshFunctionIndex();
		context.RefreshInlineCycles();
		bool changed = false;
		for(size_t i = 0; i < program.functions.size(); i++)
			if(program.functions[i].is_definition)
				changed |= OptimizeFunction(program.functions[i],
				                            context);
		if(!changed)
			break;
	}
}
