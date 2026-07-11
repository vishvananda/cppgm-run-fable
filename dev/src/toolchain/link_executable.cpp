#include "toolchain/link_executable.h"

#include <map>
#include <set>
#include <stdexcept>

#include "mir_model.h"
#include "x86/mir_to_native.h"

using std::map;
using std::runtime_error;
using std::set;
using std::string;
using std::vector;

namespace toolchain {

namespace {

typedef mir_model::MirInstruction Ins;
typedef mir_model::MirOperand Op;

// One resolved definition: where an external name lives.
struct Definition
{
	size_t module = 0;
	size_t symbol = 0;
	bool strong = false;
};

Op MakeSymbolOp(const string & name)
{
	Op op;
	op.kind = Op::OP_SYMBOL;
	op.text = name;
	return op;
}

Op MakeRegOp(X64Register reg)
{
	Op op;
	op.kind = Op::OP_REG;
	op.reg = reg;
	return op;
}

Op MakeImmOp(long long value)
{
	Op op;
	op.kind = Op::OP_IMM;
	op.imm = value;
	return op;
}

Op MakeGlobalOp(const string & name)
{
	Op op;
	op.kind = Op::OP_GLOBAL;
	op.text = name;
	return op;
}

Op MakeDerefOp(X64Register reg, long long offset)
{
	Op op;
	op.kind = Op::OP_DEREF;
	op.reg = reg;
	op.offset = offset;
	return op;
}

Op MakeLabelOp(const string & label)
{
	Op op;
	op.kind = Op::OP_LABEL;
	op.text = label;
	return op;
}

Ins MakeIns(Ins::Opcode opcode)
{
	Ins ins;
	ins.opcode = opcode;
	return ins;
}

Ins MakeCall(const string & callee)
{
	Ins ins = MakeIns(Ins::MI_CALL);
	ins.operands.push_back(MakeSymbolOp(callee));
	return ins;
}

Ins MakeMovRegReg(X64Register dst, X64Register src)
{
	Ins ins = MakeIns(Ins::MI_MOV);
	ins.operands.push_back(MakeRegOp(dst));
	ins.operands.push_back(MakeRegOp(src));
	return ins;
}

// The program startup: run every unit's dynamic initializers in link
// order, run the entry, carry its result across the finalizers (r12 is
// callee-saved), and exit with it.
mir_model::MirFunction BuildStartFunction(const vector<string> & inits,
                                          const string & entry,
                                          const vector<string> & finis)
{
	mir_model::MirFunction start;
	start.name = "__cppgm_start";
	start.return_type = "void";
	mir_model::MirBlock block;
	block.label = "entry";
	for (size_t i = 0; i < inits.size(); i++)
		block.instructions.push_back(MakeCall(inits[i]));
	block.instructions.push_back(MakeCall(entry));
	if (finis.empty())
	{
		block.instructions.push_back(MakeMovRegReg(XR_RDI, XR_RAX));
	}
	else
	{
		block.instructions.push_back(MakeMovRegReg(XR_R12, XR_RAX));
		for (size_t i = finis.size(); i > 0; i--)
			block.instructions.push_back(MakeCall(finis[i - 1]));
		block.instructions.push_back(MakeMovRegReg(XR_RDI, XR_R12));
	}
	block.instructions.push_back(MakeIns(Ins::MI_EXIT));
	start.blocks.push_back(block);
	return start;
}

// The unwind entry: pop the innermost handler record {prev, dispatch,
// rbp, rsp}, reset the handler selector, restore the recorded frame,
// and jump into the record's dispatch block. Dispatch blocks pop any
// further same-frame regions themselves with frame-bounded eh_end
// markers. With no installed handler the exception is unhandled and
// the program exits with status 134.
mir_model::MirFunction BuildUnwindRaiseFunction()
{
	mir_model::MirFunction raise;
	raise.name = "__cppgm_unwind_raise";
	raise.return_type = "void";

	mir_model::MirBlock entry;
	entry.label = "entry";
	Ins load_top = MakeIns(Ins::MI_LOAD);
	load_top.type = "i64";
	load_top.operands.push_back(MakeRegOp(XR_RAX));
	load_top.operands.push_back(MakeGlobalOp("__cppgm_eh_top"));
	entry.instructions.push_back(load_top);
	Ins compare = MakeIns(Ins::MI_CMP);
	compare.type = "i64";
	compare.operands.push_back(MakeRegOp(XR_RAX));
	compare.operands.push_back(MakeImmOp(0));
	entry.instructions.push_back(compare);
	Ins branch = MakeIns(Ins::MI_JCC);
	branch.condition = XC_E;
	branch.operands.push_back(MakeLabelOp("unhandled"));
	entry.instructions.push_back(branch);

	// selector = 0 (the dispatch markers fill it in)
	Ins zero = MakeIns(Ins::MI_MOV);
	zero.operands.push_back(MakeRegOp(XR_RCX));
	zero.operands.push_back(MakeImmOp(0));
	entry.instructions.push_back(zero);
	Ins reset = MakeIns(Ins::MI_STORE);
	reset.type = "i64";
	reset.operands.push_back(MakeGlobalOp("__cppgm_eh_selector"));
	reset.operands.push_back(MakeRegOp(XR_RCX));
	entry.instructions.push_back(reset);

	// top = record.prev (the landed record leaves the chain)
	Ins prev = MakeIns(Ins::MI_LOAD);
	prev.type = "i64";
	prev.operands.push_back(MakeRegOp(XR_RDX));
	prev.operands.push_back(MakeDerefOp(XR_RAX, 0));
	entry.instructions.push_back(prev);
	Ins store_top = MakeIns(Ins::MI_STORE);
	store_top.type = "i64";
	store_top.operands.push_back(MakeGlobalOp("__cppgm_eh_top"));
	store_top.operands.push_back(MakeRegOp(XR_RDX));
	entry.instructions.push_back(store_top);

	// dispatch target, then the recorded rbp/rsp, then the jump
	Ins dispatch = MakeIns(Ins::MI_LOAD);
	dispatch.type = "i64";
	dispatch.operands.push_back(MakeRegOp(XR_RCX));
	dispatch.operands.push_back(MakeDerefOp(XR_RAX, 8));
	entry.instructions.push_back(dispatch);
	Ins frame = MakeIns(Ins::MI_LOAD);
	frame.type = "i64";
	frame.operands.push_back(MakeRegOp(XR_RBP));
	frame.operands.push_back(MakeDerefOp(XR_RAX, 16));
	entry.instructions.push_back(frame);
	Ins stack = MakeIns(Ins::MI_LOAD);
	stack.type = "i64";
	stack.operands.push_back(MakeRegOp(XR_RSP));
	stack.operands.push_back(MakeDerefOp(XR_RAX, 24));
	entry.instructions.push_back(stack);
	Ins jump = MakeIns(Ins::MI_JMP_INDIRECT);
	jump.operands.push_back(MakeRegOp(XR_RCX));
	entry.instructions.push_back(jump);
	raise.blocks.push_back(entry);

	mir_model::MirBlock unhandled;
	unhandled.label = "unhandled";
	Ins status = MakeIns(Ins::MI_MOV);
	status.operands.push_back(MakeRegOp(XR_RDI));
	status.operands.push_back(MakeImmOp(134));
	unhandled.instructions.push_back(status);
	unhandled.instructions.push_back(MakeIns(Ins::MI_EXIT));
	raise.blocks.push_back(unhandled);
	return raise;
}

// exit(status): the C-callable process-exit primitive the runtime
// library uses (status already rides in rdi per the call ABI).
mir_model::MirFunction BuildExitFunction()
{
	mir_model::MirFunction exit_fn;
	exit_fn.name = "__cppgm_exit";
	exit_fn.return_type = "void";
	mir_model::MirBlock block;
	block.label = "entry";
	block.instructions.push_back(MakeIns(Ins::MI_EXIT));
	exit_fn.blocks.push_back(block);
	return exit_fn;
}

mir_model::GlobalDefinition MakeZeroGlobal(const string & name)
{
	mir_model::GlobalDefinition global;
	global.name = name;
	global.storage_kind = mir_model::GlobalDefinition::GS_SCALAR;
	global.init_kind = mir_model::GlobalDefinition::GI_ZERO;
	global.type = "i64";
	return global;
}

// Encodes the synthesized start/unwind module. Its first item is the
// start function, which the layout places at image item 0 (the entry
// point). It owns the exception-chain globals the generated EH code
// and the runtime library share, so cleanup-only programs link without
// the runtime library. External references (init/fini hooks by
// module-qualified name) resolve like any other undefined symbols.
ObjectModule BuildSupportModule(const vector<string> & inits,
                                const string & entry,
                                const vector<string> & finis,
                                const string & target)
{
	mir_model::MirProgram program;
	program.target = target;
	program.functions.push_back(
		BuildStartFunction(inits, entry, finis));
	program.functions.push_back(BuildUnwindRaiseFunction());
	program.functions.push_back(BuildExitFunction());
	program.globals.push_back(MakeZeroGlobal("__cppgm_eh_top"));
	program.globals.push_back(MakeZeroGlobal("__cppgm_eh_selector"));
	program.globals.push_back(MakeZeroGlobal("__cppgm_eh_exception"));

	NativeModule native = EncodeMirProgramModule(program);
	ObjectModule module;
	module.target = target;
	module.items = native.items;
	for (size_t i = 0; i < native.label_names.size(); i++)
	{
		ObjectSymbol symbol;
		symbol.low_name = native.label_names[i];
		symbol.item = native.label_items[i];
		if (symbol.item >= 0)
		{
			bool internal = symbol.low_name == "__cppgm_start" ||
			                symbol.low_name.empty();
			symbol.binding = internal ? ObjectSymbol::SB_INTERNAL
			                          : ObjectSymbol::SB_STRONG;
			symbol.external_name = internal ? string() : symbol.low_name;
		}
		else
		{
			symbol.binding = ObjectSymbol::SB_UNDEFINED;
			symbol.external_name = symbol.low_name;
		}
		module.symbols.push_back(symbol);
	}
	return module;
}

// Cross-module resolution table over strong and weak definitions.
// Duplicate strong definitions are the PA29 duplicate-symbol error;
// the first weak definition wins when no strong one exists.
map<string, Definition> BuildDefinitionTable(
	const vector<LinkInput> & inputs)
{
	map<string, Definition> definitions;
	for (size_t m = 0; m < inputs.size(); m++)
	{
		const ObjectModule & module = inputs[m].module;
		for (size_t s = 0; s < module.symbols.size(); s++)
		{
			const ObjectSymbol & symbol = module.symbols[s];
			if (symbol.item < 0 || symbol.external_name.empty() ||
			    (symbol.binding != ObjectSymbol::SB_STRONG &&
			     symbol.binding != ObjectSymbol::SB_WEAK))
				continue;
			Definition candidate;
			candidate.module = m;
			candidate.symbol = s;
			candidate.strong = symbol.binding == ObjectSymbol::SB_STRONG;
			map<string, Definition>::iterator existing =
				definitions.find(symbol.external_name);
			if (existing == definitions.end())
			{
				definitions[symbol.external_name] = candidate;
				continue;
			}
			if (existing->second.strong && candidate.strong)
				throw runtime_error(
					"duplicate global symbol definition: " +
					symbol.external_name + " (" +
					inputs[existing->second.module].name + ", " +
					inputs[m].name + ")");
			if (candidate.strong)
				existing->second = candidate;
		}
	}
	return definitions;
}

// The referenced-but-undefined external names of one module, resolved
// against nothing: purely which symbols its patches actually name.
void CollectReferencedUndefined(const ObjectModule & module,
                                set<size_t> & out)
{
	for (size_t i = 0; i < module.items.size(); i++)
	{
		const ImageItem & item = module.items[i];
		for (size_t p = 0; p < item.patches.size(); p++)
		{
			const X86Patch & patch = item.patches[p];
			if (!patch.imm.has_label)
				continue;
			size_t symbol = static_cast<size_t>(patch.imm.label);
			if (symbol < module.symbols.size() &&
			    module.symbols[symbol].item < 0)
				out.insert(symbol);
		}
	}
}

}  // namespace

vector<string> UnresolvedExternals(const vector<LinkInput> & inputs)
{
	map<string, Definition> definitions;
	// Weak/strong conflicts are reported by LinkExecutable; here any
	// definition satisfies a reference.
	for (size_t m = 0; m < inputs.size(); m++)
		for (size_t s = 0; s < inputs[m].module.symbols.size(); s++)
		{
			const ObjectSymbol & symbol = inputs[m].module.symbols[s];
			if (symbol.item >= 0 && !symbol.external_name.empty())
				definitions[symbol.external_name] = Definition();
		}
	vector<string> unresolved;
	set<string> seen;
	for (size_t m = 0; m < inputs.size(); m++)
	{
		set<size_t> referenced;
		CollectReferencedUndefined(inputs[m].module, referenced);
		for (set<size_t>::const_iterator it = referenced.begin();
		     it != referenced.end(); ++it)
		{
			const string & name =
				inputs[m].module.symbols[*it].external_name;
			if (!definitions.count(name) && seen.insert(name).second)
				unresolved.push_back(name);
		}
	}
	return unresolved;
}

void LinkExecutable(const vector<LinkInput> & inputs,
                    const string & outfile, const string & target)
{
	for (size_t m = 0; m < inputs.size(); m++)
		if (inputs[m].module.target != target)
			throw runtime_error(
				"linked inputs must target the same native backend "
				"target: " + inputs[m].name);

	// The program hooks: entry (exactly one), init/fini per module.
	// Internal hook symbols are reached through synthesized external
	// names so the support module's calls can resolve to them.
	vector<string> inits;
	vector<string> finis;
	string entry_name;
	vector<LinkInput> linked = inputs;
	for (size_t m = 0; m < linked.size(); m++)
	{
		ObjectModule & module = linked[m].module;
		if (module.entry_symbol >= 0)
		{
			if (!entry_name.empty())
				throw runtime_error(
					"duplicate program entry (main) in " +
					linked[m].name);
			entry_name = "__cppgm_hook_entry";
			module.symbols[module.entry_symbol].external_name =
				entry_name;
			module.symbols[module.entry_symbol].binding =
				ObjectSymbol::SB_STRONG;
		}
		if (module.init_symbol >= 0)
		{
			string name = "__cppgm_hook_init_" + std::to_string(m);
			module.symbols[module.init_symbol].external_name = name;
			module.symbols[module.init_symbol].binding =
				ObjectSymbol::SB_STRONG;
			inits.push_back(name);
		}
		if (module.fini_symbol >= 0)
		{
			string name = "__cppgm_hook_fini_" + std::to_string(m);
			module.symbols[module.fini_symbol].external_name = name;
			module.symbols[module.fini_symbol].binding =
				ObjectSymbol::SB_STRONG;
			finis.push_back(name);
		}
	}
	if (entry_name.empty())
		throw runtime_error("program has no main function");

	LinkInput support;
	support.name = "<cppgm++ startup>";
	support.module =
		BuildSupportModule(inits, entry_name, finis, target);
	linked.insert(linked.begin(), support);

	map<string, Definition> definitions = BuildDefinitionTable(linked);

	// Global item order: module order, items in module order. Code
	// items align to cache lines so no entry sleds are inserted and
	// symbol+offset patches land exactly.
	vector<size_t> item_base(linked.size(), 0);
	size_t total_items = 0;
	for (size_t m = 0; m < linked.size(); m++)
	{
		item_base[m] = total_items;
		total_items += linked[m].module.items.size();
	}

	ProgramImage image;
	image.SetLabelCount(static_cast<int>(total_items));
	for (size_t m = 0; m < linked.size(); m++)
	{
		const ObjectModule & module = linked[m].module;
		for (size_t i = 0; i < module.items.size(); i++)
		{
			ImageItem item = module.items[i];
			if (item.is_code && item.align < 64)
				item.align = 64;
			// Rewrite patches from module-local symbol indices to
			// global item labels, folding in symbol offsets.
			for (size_t p = 0; p < item.patches.size(); p++)
			{
				X86Patch & patch = item.patches[p];
				if (!patch.imm.has_label)
					continue;
				const ObjectSymbol * symbol =
					&module.symbols[static_cast<size_t>(
						patch.imm.label)];
				size_t target_module = m;
				if (symbol->item < 0)
				{
					map<string, Definition>::const_iterator found =
						definitions.find(symbol->external_name);
					if (found == definitions.end())
						throw runtime_error(
							"unresolved external symbol: " +
							symbol->external_name + " (" +
							linked[m].name + ")");
					target_module = found->second.module;
					symbol = &linked[target_module].module
						.symbols[found->second.symbol];
				}
				patch.imm.label = static_cast<int>(
					item_base[target_module] +
					static_cast<size_t>(symbol->item));
				patch.imm.addend += static_cast<unsigned long long>(
					symbol->offset);
			}
			image.AddItem(item);
		}
	}
	for (size_t i = 0; i < total_items; i++)
		image.BindLabel(static_cast<int>(i), i);
	image.WriteExecutable(outfile);
}

}  // namespace toolchain
