// PA37 driver-surface merge: `cppgm++ --emit-lowir -g<n>` lowers each
// translation unit separately, then merges the parsed units here before
// the shared optimizer runs. The reference driver resolves the merged
// symbol table like a linker would, so vague-linkage definitions that
// repeat across units collapse to the first copy.

#include "lowir/lowir_merge.h"

#include <set>
#include <stdexcept>

using std::set;

namespace {

bool is_weak(const LowIRMetadata & metadata)
{
	return metadata.find("binding") == "weak";
}

bool is_internal(const LowIRMetadata & metadata)
{
	return metadata.find("binding") == "internal";
}

void rename_operand(LowIROperand & operand,
                    const map<string, string> & renames)
{
	if(operand.kind != LOWIR_OPERAND_GLOBAL)
		return;
	map<string, string>::const_iterator found =
		renames.find(operand.name);
	if(found != renames.end())
		operand.name = found->second;
}

void rename_symbol(string & name, const map<string, string> & renames)
{
	map<string, string>::const_iterator found = renames.find(name);
	if(found != renames.end())
		name = found->second;
}

// Rewrites every top-level symbol reference in one unit. `tls_for` is
// the only metadata key whose value names a symbol (as `@name`).
void apply_renames(LowIRProgram & unit, const map<string, string> & renames)
{
	for(size_t f = 0; f < unit.functions.size(); f++)
	{
		LowIRFunction & fn = unit.functions[f];
		rename_symbol(fn.name, renames);
		for(size_t m = 0; m < fn.metadata.items.size(); m++)
		{
			std::pair<string, string> & item = fn.metadata.items[m];
			if(item.first != "tls_for" || item.second.size() < 2 ||
			   item.second[0] != '@')
				continue;
			string target = item.second.substr(1);
			rename_symbol(target, renames);
			item.second = "@" + target;
		}
		for(size_t b = 0; b < fn.blocks.size(); b++)
			for(size_t k = 0; k < fn.blocks[b].instructions.size(); k++)
			{
				LowIRInstruction & ins = fn.blocks[b].instructions[k];
				if(ins.opcode == LOWIR_INS_CALL && !ins.callee_is_temp)
					rename_symbol(ins.callee, renames);
				for(size_t o = 0; o < ins.operands.size(); o++)
					rename_operand(ins.operands[o], renames);
				for(size_t o = 0; o < ins.switch_values.size(); o++)
					rename_operand(ins.switch_values[o], renames);
				for(size_t t = 0; t < ins.eh_types.size(); t++)
					rename_symbol(ins.eh_types[t], renames);
			}
	}
	for(size_t g = 0; g < unit.globals.size(); g++)
	{
		LowIRGlobal & global = unit.globals[g];
		rename_symbol(global.name, renames);
		if(global.init == LOWIR_GLOBAL_ADDR)
			rename_symbol(global.addr_symbol, renames);
		for(size_t i = 0; i < global.items.size(); i++)
			if(global.items[i].kind == LOWIR_DATA_ADDR)
				rename_symbol(global.items[i].symbol, renames);
	}
	for(size_t a = 0; a < unit.aliases.size(); a++)
		rename_symbol(unit.aliases[a].target, renames);
}

// Internal-binding definitions are unit-scoped: when another unit uses
// the same top-level name for anything, the internal symbol takes a
// fresh per-unit spelling so the merged program keeps both objects.
void RenameCollidingInternals(vector<LowIRProgram> & units)
{
	map<string, set<size_t> > mentioned;
	for(size_t u = 0; u < units.size(); u++)
	{
		for(size_t g = 0; g < units[u].globals.size(); g++)
			mentioned[units[u].globals[g].name].insert(u);
		for(size_t f = 0; f < units[u].functions.size(); f++)
			mentioned[units[u].functions[f].name].insert(u);
	}

	set<string> taken;
	for(map<string, set<size_t> >::const_iterator it = mentioned.begin();
	    it != mentioned.end(); ++it)
		taken.insert(it->first);

	for(size_t u = 0; u < units.size(); u++)
	{
		map<string, string> renames;
		for(size_t g = 0; g < units[u].globals.size(); g++)
		{
			const LowIRGlobal & global = units[u].globals[g];
			if(global.is_definition && is_internal(global.metadata) &&
			   mentioned[global.name].size() > 1)
				renames[global.name] = "";
		}
		for(size_t f = 0; f < units[u].functions.size(); f++)
		{
			const LowIRFunction & fn = units[u].functions[f];
			if(fn.is_definition && is_internal(fn.metadata) &&
			   mentioned[fn.name].size() > 1)
				renames[fn.name] = "";
		}
		if(renames.empty())
			continue;
		for(map<string, string>::iterator r = renames.begin();
		    r != renames.end(); ++r)
		{
			string fresh = r->first + "__u" + std::to_string(u + 1);
			while(taken.count(fresh))
				fresh += "_";
			taken.insert(fresh);
			r->second = fresh;
		}
		apply_renames(units[u], renames);
	}
}

struct EntryRef
{
	size_t unit = 0;
	size_t index = 0;
	bool present = false;
};

struct NameResolution
{
	EntryRef definition;
	bool definition_weak = false;
	EntryRef declare;
};

void ResolveDefinition(NameResolution & entry, const string & name,
                       const LowIRMetadata & metadata,
                       size_t unit, size_t index)
{
	bool weak = is_weak(metadata);
	if(!entry.definition.present)
	{
		entry.definition.unit = unit;
		entry.definition.index = index;
		entry.definition.present = true;
		entry.definition_weak = weak;
		return;
	}
	if(weak)
		return;  // vague linkage: the first copy already won
	if(!entry.definition_weak)
		throw std::runtime_error(
			"duplicate top-level symbol: @" + name);
	// A strong definition supersedes the weak copies.
	entry.definition.unit = unit;
	entry.definition.index = index;
	entry.definition_weak = false;
}

}  // namespace

LowIRProgram MergeLowIRUnits(vector<LowIRProgram> & units)
{
	RenameCollidingInternals(units);

	map<string, NameResolution> names;
	for(size_t u = 0; u < units.size(); u++)
	{
		for(size_t g = 0; g < units[u].globals.size(); g++)
		{
			const LowIRGlobal & global = units[u].globals[g];
			NameResolution & entry = names[global.name];
			if(global.is_definition)
				ResolveDefinition(entry, global.name, global.metadata,
				                  u, g);
			else if(!entry.declare.present)
			{
				entry.declare.unit = u;
				entry.declare.index = g;
				entry.declare.present = true;
			}
		}
		for(size_t f = 0; f < units[u].functions.size(); f++)
		{
			const LowIRFunction & fn = units[u].functions[f];
			NameResolution & entry = names[fn.name];
			if(fn.is_definition)
				ResolveDefinition(entry, fn.name, fn.metadata, u, f);
			else if(!entry.declare.present)
			{
				entry.declare.unit = u;
				entry.declare.index = f;
				entry.declare.present = true;
			}
		}
	}

	LowIRProgram merged;
	set<std::pair<string, string> > alias_seen;
	for(size_t u = 0; u < units.size(); u++)
	{
		for(size_t g = 0; g < units[u].globals.size(); g++)
		{
			LowIRGlobal & global = units[u].globals[g];
			const NameResolution & entry = names[global.name];
			const EntryRef & winner = global.is_definition
				? entry.definition
				: entry.declare;
			bool keep = winner.present && winner.unit == u &&
				winner.index == g &&
				(global.is_definition || !entry.definition.present);
			if(keep)
				merged.globals.push_back(std::move(global));
		}
		for(size_t f = 0; f < units[u].functions.size(); f++)
		{
			LowIRFunction & fn = units[u].functions[f];
			const NameResolution & entry = names[fn.name];
			const EntryRef & winner = fn.is_definition
				? entry.definition
				: entry.declare;
			bool keep = winner.present && winner.unit == u &&
				winner.index == f &&
				(fn.is_definition || !entry.definition.present);
			if(keep)
				merged.functions.push_back(std::move(fn));
		}
		for(size_t a = 0; a < units[u].aliases.size(); a++)
		{
			const LowIRAlias & alias = units[u].aliases[a];
			if(alias_seen.insert(std::make_pair(alias.object_symbol,
			                                    alias.target)).second)
				merged.aliases.push_back(alias);
		}
	}
	return merged;
}
