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
// callee-saved), and exit with it. rbp is zeroed before the prologue
// so the frame chain the private unwinder walks has a terminating
// sentinel.
mir_model::MirFunction BuildStartFunction(const vector<string> & inits,
                                          const string & entry,
                                          const vector<string> & finis)
{
	mir_model::MirFunction start;
	start.name = "__cppgm_start";
	start.return_type = "void";
	start.zero_frame_pointer = true;
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

// The frame-chain primitive: returns the caller's rbp (its own [rbp]
// after the standard prologue), the runtime walker's starting frame.
mir_model::MirFunction BuildGetFrameFunction()
{
	mir_model::MirFunction get_frame;
	get_frame.name = "__cppgm_get_frame";
	get_frame.return_type = "i64";
	mir_model::MirBlock block;
	block.label = "entry";
	Ins load = MakeIns(Ins::MI_LOAD);
	load.type = "i64";
	load.operands.push_back(MakeRegOp(XR_RAX));
	load.operands.push_back(MakeDerefOp(XR_RBP, 0));
	block.instructions.push_back(load);
	Ins ret = MakeIns(Ins::MI_RET);
	ret.operands.push_back(MakeRegOp(XR_RAX));
	block.instructions.push_back(ret);
	get_frame.blocks.push_back(block);
	return get_frame;
}

// The landing primitive: __cppgm_land(lp, rbp, cookie, selector)
// installs the Itanium landing contract (cookie in rax, selector in
// edx), restores the target frame's rbp, and jumps to the landing
// pad, whose entry re-establishes rsp itself.
mir_model::MirFunction BuildLandFunction()
{
	mir_model::MirFunction land;
	land.name = "__cppgm_land";
	land.return_type = "void";
	mir_model::MirBlock block;
	block.label = "entry";
	block.instructions.push_back(MakeMovRegReg(XR_RAX, XR_RDX));
	block.instructions.push_back(MakeMovRegReg(XR_RDX, XR_RCX));
	block.instructions.push_back(MakeMovRegReg(XR_RBP, XR_RSI));
	Ins jump = MakeIns(Ins::MI_JMP_INDIRECT);
	jump.operands.push_back(MakeRegOp(XR_RDI));
	block.instructions.push_back(jump);
	land.blocks.push_back(block);
	return land;
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

// Encodes the synthesized start/land module. Its first item is the
// start function, which the layout places at image item 0 (the entry
// point). It owns the landing/frame primitives and the pointer slot
// the linker later aims at the synthesized region table. External
// references (init/fini hooks by module-qualified name) resolve like
// any other undefined symbols.
ObjectModule BuildSupportModule(const vector<string> & inits,
                                const string & entry,
                                const vector<string> & finis,
                                const string & target)
{
	mir_model::MirProgram program;
	program.target = target;
	program.functions.push_back(
		BuildStartFunction(inits, entry, finis));
	program.functions.push_back(BuildGetFrameFunction());
	program.functions.push_back(BuildLandFunction());
	program.functions.push_back(BuildExitFunction());
	program.globals.push_back(MakeZeroGlobal("__cppgm_eh_region_table"));

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

// True when any module's patches - or its typed catch-clause type
// references, which carry no patches of their own - name an external
// the definition table cannot satisfy; the trigger for the on-demand
// runtime-library module.
bool HasUnresolvedReference(const vector<LinkInput> & linked,
                            const map<string, Definition> & definitions)
{
	for (size_t m = 0; m < linked.size(); m++)
	{
		const ObjectModule & module = linked[m].module;
		set<size_t> referenced;
		CollectReferencedUndefined(module, referenced);
		for (size_t f = 0; f < module.eh_functions.size(); f++)
		{
			const EhFunctionInfo & eh = module.eh_functions[f];
			for (size_t c = 0; c < eh.chains.size(); c++)
				for (size_t a = 0; a < eh.chains[c].size(); a++)
					if (eh.chains[c][a].kind == EhAction::EH_CATCH)
						referenced.insert(static_cast<size_t>(
							eh.chains[c][a].type_symbol));
		}
		for (set<size_t>::const_iterator it = referenced.begin();
		     it != referenced.end(); ++it)
			if (module.symbols[*it].item < 0 &&
			    !definitions.count(module.symbols[*it].external_name))
				return true;
	}
	return false;
}

// Resolves one module symbol to a (global item label, offset) pair,
// following undefined externals through the definition table.
void ResolveSymbolTarget(const vector<LinkInput> & linked,
                         const vector<size_t> & item_base,
                         const map<string, Definition> & definitions,
                         size_t module_index, int symbol_index,
                         int & out_label, long long & out_offset)
{
	const ObjectSymbol * symbol =
		&linked[module_index].module.symbols[
			static_cast<size_t>(symbol_index)];
	size_t target_module = module_index;
	if (symbol->item < 0)
	{
		map<string, Definition>::const_iterator found =
			definitions.find(symbol->external_name);
		if (found == definitions.end())
			throw runtime_error("unresolved external symbol: " +
			                    symbol->external_name + " (" +
			                    linked[module_index].name + ")");
		target_module = found->second.module;
		symbol = &linked[target_module].module
			.symbols[found->second.symbol];
	}
	out_label = static_cast<int>(item_base[target_module] +
	                             static_cast<size_t>(symbol->item));
	out_offset = symbol->offset;
}

void AppendTableWord(ImageItem & item, unsigned long long value)
{
	for (int b = 0; b < 8; b++)
		item.bytes.push_back(static_cast<unsigned char>(value >> (8 * b)));
}

void AppendTableAddress(ImageItem & item, int label, long long offset)
{
	X86Patch patch;
	patch.offset = item.bytes.size();
	patch.size = 8;
	patch.kind = X86_PATCH_ABS;
	patch.imm = X86Imm::Label(label,
	                          static_cast<unsigned long long>(offset));
	item.patches.push_back(patch);
	AppendTableWord(item, 0);
}

// True when a chain carries no catch actions - the row is a pure
// cleanup and its action pointer is null, mirroring an LSDA call-site
// row with action 0. Exception-spec records count as cleanup-like:
// the private runtime has no unexpected machinery, so specs stay
// inert (the semantics whole-program mode has always had).
bool ChainIsPureCleanup(const vector<EhAction> & chain)
{
	for (size_t a = 0; a < chain.size(); a++)
		if (chain[a].kind != EhAction::EH_CLEANUP &&
		    chain[a].kind != EhAction::EH_SPEC)
			return false;
	return true;
}

// The record count the private table stores for a chain: every action
// except the ignored exception-spec filters.
size_t ChainRecordActions(const vector<EhAction> & chain)
{
	size_t count = 0;
	for (size_t a = 0; a < chain.size(); a++)
		if (chain[a].kind != EhAction::EH_SPEC)
			count++;
	return count;
}

// The private unwinder's flat region table, synthesized in global
// label space after patch rewriting: [row count][rows {start, end,
// landing pad, action pointer}][action records {kind, filter,
// type_info} terminated by a zero kind]. Rows come straight from the
// modules' typed host-EH facts - the same facts object emission
// renders into .eh_frame/.gcc_except_table.
ImageItem BuildEhRegionTable(const vector<LinkInput> & linked,
                             const vector<size_t> & item_base,
                             const map<string, Definition> & definitions,
                             int self_label)
{
	size_t row_count = 0;
	for (size_t m = 0; m < linked.size(); m++)
	{
		const ObjectModule & module = linked[m].module;
		for (size_t f = 0; f < module.eh_functions.size(); f++)
			row_count += module.eh_functions[f].call_sites.size();
	}

	ImageItem item;
	item.align = 8;
	AppendTableWord(item, row_count);
	size_t record_cursor = (1 + 4 * row_count) * 8;

	// Rows first; chains get record offsets in first-reference order
	// and are emitted in exactly that order afterwards.
	struct ChainRef
	{
		size_t module;
		const vector<EhAction> * chain;
	};
	std::map<std::pair<size_t, std::pair<size_t, size_t> >, size_t>
		chain_offsets;
	vector<ChainRef> record_layout;
	for (size_t m = 0; m < linked.size(); m++)
	{
		const ObjectModule & module = linked[m].module;
		for (size_t f = 0; f < module.eh_functions.size(); f++)
		{
			const EhFunctionInfo & eh = module.eh_functions[f];
			int code_label = static_cast<int>(
				item_base[m] + static_cast<size_t>(eh.item));
			for (size_t s = 0; s < eh.call_sites.size(); s++)
			{
				const EhCallSite & site = eh.call_sites[s];
				long long base = static_cast<long long>(eh.item_offset);
				AppendTableAddress(item, code_label,
				                   base +
				                   static_cast<long long>(site.start));
				AppendTableAddress(item, code_label,
				                   base +
				                   static_cast<long long>(site.start +
				                                          site.length));
				AppendTableAddress(item, code_label,
				                   base +
				                   static_cast<long long>(
				                       site.landing_pad));
				size_t chain = static_cast<size_t>(site.chain);
				if (site.chain < 0 ||
				    ChainIsPureCleanup(eh.chains[chain]))
				{
					AppendTableWord(item, 0);
					continue;
				}
				std::pair<size_t, std::pair<size_t, size_t> > key(
					m, std::make_pair(f, chain));
				if (!chain_offsets.count(key))
				{
					chain_offsets[key] = record_cursor;
					record_cursor +=
						3 * 8 *
						(ChainRecordActions(eh.chains[chain]) + 1);
					ChainRef ref;
					ref.module = m;
					ref.chain = &eh.chains[chain];
					record_layout.push_back(ref);
				}
				AppendTableAddress(
					item, self_label,
					static_cast<long long>(chain_offsets[key]));
			}
		}
	}
	for (size_t r = 0; r < record_layout.size(); r++)
	{
		const vector<EhAction> & chain = *record_layout[r].chain;
		for (size_t a = 0; a < chain.size(); a++)
		{
			const EhAction & action = chain[a];
			if (action.kind == EhAction::EH_SPEC)
				continue;
			if (action.kind == EhAction::EH_CLEANUP)
			{
				AppendTableWord(item, 3);
				AppendTableWord(item, 0);
				AppendTableWord(item, 0);
				continue;
			}
			AppendTableWord(
				item, action.kind == EhAction::EH_CATCH_ALL ? 2 : 1);
			AppendTableWord(item, static_cast<unsigned long long>(
				action.filter));
			if (action.kind == EhAction::EH_CATCH)
			{
				int label = 0;
				long long offset = 0;
				ResolveSymbolTarget(linked, item_base, definitions,
				                    record_layout[r].module,
				                    action.type_symbol, label, offset);
				AppendTableAddress(item, label, offset);
			}
			else
			{
				AppendTableWord(item, 0);
			}
		}
		AppendTableWord(item, 0);
		AppendTableWord(item, 0);
		AppendTableWord(item, 0);
	}
	if (item.bytes.size() != record_cursor)
		throw runtime_error("eh region table layout mismatch");
	return item;
}

// The program hooks: entry (exactly one), init/fini per module.
// Internal hook symbols are reached through synthesized external
// names so the support module's calls can resolve to them.
struct ProgramHooks
{
	vector<string> inits;
	vector<string> finis;
	string entry;
};

ProgramHooks ExternalizeProgramHooks(vector<LinkInput> & linked)
{
	ProgramHooks hooks;
	for (size_t m = 0; m < linked.size(); m++)
	{
		ObjectModule & module = linked[m].module;
		if (module.entry_symbol >= 0)
		{
			if (!hooks.entry.empty())
				throw runtime_error(
					"duplicate program entry (main) in " +
					linked[m].name);
			hooks.entry = "__cppgm_hook_entry";
			module.symbols[module.entry_symbol].external_name =
				hooks.entry;
			module.symbols[module.entry_symbol].binding =
				ObjectSymbol::SB_STRONG;
		}
		if (module.init_symbol >= 0)
		{
			string name = "__cppgm_hook_init_" + std::to_string(m);
			module.symbols[module.init_symbol].external_name = name;
			module.symbols[module.init_symbol].binding =
				ObjectSymbol::SB_STRONG;
			hooks.inits.push_back(name);
		}
		if (module.fini_symbol >= 0)
		{
			string name = "__cppgm_hook_fini_" + std::to_string(m);
			module.symbols[module.fini_symbol].external_name = name;
			module.symbols[module.fini_symbol].binding =
				ObjectSymbol::SB_STRONG;
			hooks.finis.push_back(name);
		}
	}
	if (hooks.entry.empty())
		throw runtime_error("program has no main function");
	return hooks;
}

}  // namespace

void LinkExecutable(vector<LinkInput> linked, const string & outfile,
                    const string & target,
                    RuntimeModuleSupplier runtime_supplier)
{
	for (size_t m = 0; m < linked.size(); m++)
		if (linked[m].module.target != target)
			throw runtime_error(
				"linked inputs must target the same native backend "
				"target: " + linked[m].name);

	ProgramHooks hooks = ExternalizeProgramHooks(linked);

	LinkInput support;
	support.name = "<cppgm++ startup>";
	support.module = BuildSupportModule(hooks.inits, hooks.entry,
	                                    hooks.finis, target);
	linked.insert(linked.begin(), support);

	map<string, Definition> definitions = BuildDefinitionTable(linked);

	// Runtime names (EH entry points, RTTI anchors, operator new) come
	// from the built-in runtime library, compiled once through the
	// ordinary pipeline when the link still needs definitions. The
	// support module already owns the exception-chain globals, so
	// cleanup-only programs link without it.
	if (runtime_supplier && HasUnresolvedReference(linked, definitions))
	{
		linked.push_back(runtime_supplier(target));
		definitions = BuildDefinitionTable(linked);
	}

	// Global item order: module order, items in module order, then the
	// synthesized region table. Code items align to cache lines so no
	// entry sleds are inserted and symbol+offset patches land exactly.
	vector<size_t> item_base(linked.size(), 0);
	size_t total_items = 0;
	for (size_t m = 0; m < linked.size(); m++)
	{
		item_base[m] = total_items;
		total_items += linked[m].module.items.size();
	}

	for (size_t m = 0; m < linked.size(); m++)
	{
		ObjectModule & module = linked[m].module;
		for (size_t i = 0; i < module.items.size(); i++)
		{
			ImageItem & item = module.items[i];
			if (item.is_code && item.align < 64)
				item.align = 64;
			// Rewrite patches from module-local symbol indices to
			// global item labels, folding in symbol offsets.
			for (size_t p = 0; p < item.patches.size(); p++)
			{
				X86Patch & patch = item.patches[p];
				if (!patch.imm.has_label)
					continue;
				int label = 0;
				long long offset = 0;
				ResolveSymbolTarget(linked, item_base, definitions, m,
				                    patch.imm.label, label, offset);
				patch.imm.label = label;
				patch.imm.addend +=
					static_cast<unsigned long long>(offset);
			}
		}
	}

	// The private unwinder's region table, plus the support module's
	// pointer slot aimed at it (both already in global label space).
	int table_label = static_cast<int>(total_items);
	ImageItem region_table =
		BuildEhRegionTable(linked, item_base, definitions, table_label);
	{
		ObjectModule & support = linked[0].module;
		bool found = false;
		for (size_t s = 0; s < support.symbols.size(); s++)
		{
			const ObjectSymbol & symbol = support.symbols[s];
			if (symbol.external_name != "__cppgm_eh_region_table" ||
			    symbol.item < 0)
				continue;
			X86Patch patch;
			patch.offset = static_cast<size_t>(symbol.offset);
			patch.size = 8;
			patch.kind = X86_PATCH_ABS;
			patch.imm = X86Imm::Label(table_label, 0);
			support.items[static_cast<size_t>(symbol.item)]
				.patches.push_back(patch);
			found = true;
			break;
		}
		if (!found)
			throw runtime_error(
				"support module lost the eh region table slot");
	}

	ProgramImage image;
	image.SetLabelCount(static_cast<int>(total_items) + 1);
	for (size_t m = 0; m < linked.size(); m++)
		for (size_t i = 0; i < linked[m].module.items.size(); i++)
			image.AddItem(linked[m].module.items[i]);
	image.AddItem(region_table);
	for (size_t i = 0; i <= total_items; i++)
		image.BindLabel(static_cast<int>(i), i);
	image.WriteExecutable(outfile);
}

}  // namespace toolchain
