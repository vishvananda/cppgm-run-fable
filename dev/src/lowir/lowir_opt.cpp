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
// an inline candidate. One iterative Tarjan SCC pass: members of a
// multi-node component, plus self-loop nodes, are cyclic.
void LowIROptContext::RefreshInlineCycles()
{
	inline_cycle.clear();
	vector<const LowIRFunction *> defs;
	map<string, int> ids;
	for(size_t i = 0; i < program->functions.size(); i++)
		if(program->functions[i].is_definition)
		{
			ids[program->functions[i].name] =
				static_cast<int>(defs.size());
			defs.push_back(&program->functions[i]);
		}
	size_t count = defs.size();
	vector<vector<int> > out(count);
	vector<bool> self_loop(count, false);
	for(size_t i = 0; i < count; i++)
		for(size_t b = 0; b < defs[i]->blocks.size(); b++)
			for(size_t k = 0;
			    k < defs[i]->blocks[b].instructions.size(); k++)
			{
				const LowIRInstruction & ins =
					defs[i]->blocks[b].instructions[k];
				if(ins.opcode != LOWIR_INS_CALL || ins.callee_is_temp)
					continue;
				map<string, int>::const_iterator target =
					ids.find(ins.callee);
				if(target == ids.end())
					continue;
				if(target->second == static_cast<int>(i))
					self_loop[i] = true;
				else
					out[i].push_back(target->second);
			}

	vector<int> index(count, -1), low(count, 0);
	vector<bool> on_stack(count, false);
	vector<int> scc_stack;
	int next_index = 0;
	// Explicit DFS frames: node and next out-edge to visit.
	vector<std::pair<int, size_t> > frames;
	for(size_t root = 0; root < count; root++)
	{
		if(index[root] >= 0)
			continue;
		frames.push_back(std::make_pair(static_cast<int>(root),
		                                (size_t)0));
		while(!frames.empty())
		{
			int at = frames.back().first;
			size_t & edge = frames.back().second;
			if(edge == 0)
			{
				index[at] = low[at] = next_index++;
				scc_stack.push_back(at);
				on_stack[at] = true;
			}
			bool descended = false;
			while(edge < out[at].size())
			{
				int next = out[at][edge++];
				if(index[next] < 0)
				{
					frames.push_back(std::make_pair(next, (size_t)0));
					descended = true;
					break;
				}
				if(on_stack[next] && index[next] < low[at])
					low[at] = index[next];
			}
			if(descended)
				continue;
			if(low[at] == index[at])
			{
				// Pop the component rooted at `at`.
				vector<int> members;
				for(;;)
				{
					int member = scc_stack.back();
					scc_stack.pop_back();
					on_stack[member] = false;
					members.push_back(member);
					if(member == at)
						break;
				}
				if(members.size() > 1)
					for(size_t m = 0; m < members.size(); m++)
						inline_cycle.insert(defs[members[m]]->name);
			}
			frames.pop_back();
			if(!frames.empty())
			{
				int parent = frames.back().first;
				if(low[at] < low[parent])
					low[parent] = low[at];
			}
		}
	}
	for(size_t i = 0; i < count; i++)
		if(self_loop[i])
			inline_cycle.insert(defs[i]->name);
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

namespace {

// The single owner of "what references a LowIR symbol": direct
// callees, global operands (including switch case values), eh_types,
// and the tls_for metadata target. Counts are per occurrence so the
// weak-discard worklist can decrement as bodies drop out.
void CountFunctionSymbolRefs(const LowIRFunction & fn,
                             map<string, long> & counts)
{
	string tls = fn.metadata.find("tls_for");
	if(tls.size() > 1 && tls[0] == '@')
		counts[tls.substr(1)]++;
	for(size_t b = 0; b < fn.blocks.size(); b++)
		for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
		{
			const LowIRInstruction & ins = fn.blocks[b].instructions[k];
			if(ins.opcode == LOWIR_INS_CALL && !ins.callee_is_temp)
				counts[ins.callee]++;
			for(size_t o = 0; o < ins.operands.size(); o++)
				if(ins.operands[o].kind == LOWIR_OPERAND_GLOBAL)
					counts[ins.operands[o].name]++;
			for(size_t o = 0; o < ins.switch_values.size(); o++)
				if(ins.switch_values[o].kind == LOWIR_OPERAND_GLOBAL)
					counts[ins.switch_values[o].name]++;
			for(size_t t = 0; t < ins.eh_types.size(); t++)
				counts[ins.eh_types[t]]++;
		}
}

void CountGlobalSymbolRefs(const LowIRGlobal & global,
                           map<string, long> & counts)
{
	if(global.init == LOWIR_GLOBAL_ADDR)
		counts[global.addr_symbol]++;
	for(size_t i = 0; i < global.items.size(); i++)
		if(global.items[i].kind == LOWIR_DATA_ADDR)
			counts[global.items[i].symbol]++;
}

void CountProgramSymbolRefs(const LowIRProgram & program,
                            map<string, long> & counts)
{
	for(size_t f = 0; f < program.functions.size(); f++)
		CountFunctionSymbolRefs(program.functions[f], counts);
	for(size_t g = 0; g < program.globals.size(); g++)
		CountGlobalSymbolRefs(program.globals[g], counts);
}

}  // namespace

void PruneUnreferencedLowIRDeclares(LowIRProgram & program)
{
	map<string, long> counts;
	CountProgramSymbolRefs(program, counts);
	set<string> referenced;
	for(map<string, long>::const_iterator it = counts.begin();
	    it != counts.end(); ++it)
		referenced.insert(it->first);
	for(size_t a = 0; a < program.aliases.size(); a++)
		referenced.insert(program.aliases[a].target);

	vector<LowIRFunction> kept_functions;
	for(size_t f = 0; f < program.functions.size(); f++)
	{
		const LowIRFunction & fn = program.functions[f];
		bool keep = fn.is_definition || referenced.count(fn.name) ||
			fn.metadata.has("role") || fn.metadata.has("tls_for");
		if(keep)
			kept_functions.push_back(fn);
	}
	program.functions.swap(kept_functions);

	vector<LowIRGlobal> kept_globals;
	for(size_t g = 0; g < program.globals.size(); g++)
	{
		const LowIRGlobal & global = program.globals[g];
		bool keep = global.is_definition ||
			referenced.count(global.name) || global.metadata.has("role");
		if(keep)
			kept_globals.push_back(global);
	}
	program.globals.swap(kept_globals);
}

void RemoveUnreferencedWeakFunctions(LowIRProgram & program, int level)
{
	map<string, long> counts;
	CountProgramSymbolRefs(program, counts);

	// An alias is normally just an extra object spelling of its target
	// and dies with it (host vague-linkage discard). But when the
	// program also declares the alias's object symbol as a LowIR
	// top-level, LowIR references can reach the definition through the
	// alias, so the target stays pinned.
	set<string> declared;
	for(size_t f = 0; f < program.functions.size(); f++)
		if(!program.functions[f].is_definition)
			declared.insert(program.functions[f].name);
	for(size_t g = 0; g < program.globals.size(); g++)
		if(!program.globals[g].is_definition)
			declared.insert(program.globals[g].name);
	for(size_t a = 0; a < program.aliases.size(); a++)
		if(declared.count(program.aliases[a].object_symbol))
			counts[program.aliases[a].target]++;

	map<string, size_t> discardable;   // name -> functions index
	vector<string> worklist;
	for(size_t f = 0; f < program.functions.size(); f++)
	{
		const LowIRFunction & fn = program.functions[f];
		bool eligible = fn.is_definition &&
			fn.metadata.find("binding") == "weak" &&
			!fn.metadata.has("role") &&
			!fn.metadata.has("tls_for") &&
			fn.metadata.find("object_root") != "yes" &&
			(level >= 1 ||
			 fn.metadata.find("trivial_lifecycle") == "yes");
		if(!eligible)
			continue;
		discardable[fn.name] = f;
		if(counts[fn.name] == 0)
			worklist.push_back(fn.name);
	}
	if(discardable.empty())
		return;

	// Removing a body releases its references; newly unreferenced
	// discardable functions cascade through the worklist.
	set<string> removed;
	while(!worklist.empty())
	{
		string name = worklist.back();
		worklist.pop_back();
		if(!removed.insert(name).second)
			continue;
		map<string, long> released;
		CountFunctionSymbolRefs(
			program.functions[discardable[name]], released);
		for(map<string, long>::const_iterator it = released.begin();
		    it != released.end(); ++it)
		{
			map<string, long>::iterator count = counts.find(it->first);
			if(count == counts.end())
				continue;
			count->second -= it->second;
			if(count->second <= 0 && discardable.count(it->first) &&
			   !removed.count(it->first))
				worklist.push_back(it->first);
		}
	}
	if(removed.empty())
		return;

	vector<LowIRFunction> kept;
	for(size_t f = 0; f < program.functions.size(); f++)
		if(!removed.count(program.functions[f].name))
			kept.push_back(program.functions[f]);
	program.functions.swap(kept);
	vector<LowIRAlias> kept_aliases;
	for(size_t a = 0; a < program.aliases.size(); a++)
		if(!removed.count(program.aliases[a].target))
			kept_aliases.push_back(program.aliases[a]);
	program.aliases.swap(kept_aliases);
}

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
