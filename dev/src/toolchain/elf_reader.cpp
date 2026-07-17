#include "toolchain/elf_reader.h"

#include <map>
#include <stdexcept>
#include <vector>

#include "toolchain/eh_table.h"

using std::map;
using std::runtime_error;
using std::string;
using std::vector;

// Hand-rolled ELF64 little-endian field reads: the reader owns its own
// layout constants so host <elf.h> differences never leak in.

namespace toolchain {

namespace {

const unsigned kElfTypeRelocatable = 1;   // ET_REL
const unsigned kElfMachineX86_64 = 0x3E;  // EM_X86_64

const unsigned kSectionProgbits = 1;  // SHT_PROGBITS
const unsigned kSectionSymtab = 2;    // SHT_SYMTAB
const unsigned kSectionStrtab = 3;    // SHT_STRTAB
const unsigned kSectionRela = 4;      // SHT_RELA
const unsigned kSectionNobits = 8;    // SHT_NOBITS

const unsigned long long kFlagAlloc = 2;      // SHF_ALLOC
const unsigned long long kFlagExecinstr = 4;  // SHF_EXECINSTR

const unsigned kSymbolBindingLocal = 0;
const unsigned kSymbolBindingWeak = 2;
const unsigned kSymbolTypeSection = 3;

const unsigned kSectionIndexUndefined = 0;      // SHN_UNDEF
const unsigned kSectionIndexAbsolute = 0xfff1;  // SHN_ABS
const unsigned kSectionIndexCommon = 0xfff2;    // SHN_COMMON

const unsigned kRelocAbs64 = 1;         // R_X86_64_64
const unsigned kRelocPc32 = 2;          // R_X86_64_PC32
const unsigned kRelocGotPcrel = 9;      // R_X86_64_GOTPCREL
const unsigned kRelocPlt32 = 4;         // R_X86_64_PLT32
const unsigned kRelocAbs32 = 10;        // R_X86_64_32
const unsigned kRelocAbs32Signed = 11;  // R_X86_64_32S
const unsigned kRelocPc64 = 24;         // R_X86_64_PC64
const unsigned kRelocGotPcrelx = 41;    // R_X86_64_GOTPCRELX
const unsigned kRelocRexGotPcrelx = 42; // R_X86_64_REX_GOTPCRELX

struct ElfSection
{
	string name;
	unsigned type = 0;
	unsigned long long flags = 0;
	unsigned long long offset = 0;
	unsigned long long size = 0;
	unsigned link = 0;
	unsigned info = 0;
	unsigned long long addralign = 0;
	unsigned long long entsize = 0;
};

struct ElfFile
{
	ElfFile(const string & bytes, const string & input_name)
		: bytes(bytes), input_name(input_name)
	{
	}

	void fail(const string & what) const
	{
		throw runtime_error("bad ELF object " + input_name + ": " + what);
	}

	unsigned long long field(unsigned long long at, int size) const
	{
		if (at + static_cast<unsigned long long>(size) > bytes.size())
			fail("truncated file");
		unsigned long long value = 0;
		for (int i = 0; i < size; i++)
			value |= static_cast<unsigned long long>(
				static_cast<unsigned char>(bytes[at + i])) << (8 * i);
		return value;
	}

	const string & bytes;
	const string & input_name;
};

// One module-local GOT: an 8-byte address slot per referenced symbol,
// so RIP-relative GOT loads in host code resolve without a dynamic
// linker.
struct GotBuilder
{
	map<size_t, int> slot_symbols;  // referenced symbol -> got symbol

	int SlotSymbol(ObjectModule & module, size_t symbol)
	{
		map<size_t, int>::const_iterator found =
			slot_symbols.find(symbol);
		if (found != slot_symbols.end())
			return found->second;
		ImageItem item;
		item.align = 8;
		item.bytes.assign(8, 0);
		X86Patch patch;
		patch.offset = 0;
		patch.size = 8;
		patch.kind = X86_PATCH_ABS;
		patch.imm = X86Imm::Label(static_cast<int>(symbol), 0);
		item.patches.push_back(patch);
		module.items.push_back(item);

		ObjectSymbol slot;
		slot.low_name = "__cppgm_got_slot";
		slot.binding = ObjectSymbol::SB_INTERNAL;
		slot.item = static_cast<int>(module.items.size() - 1);
		module.symbols.push_back(slot);
		int slot_symbol = static_cast<int>(module.symbols.size() - 1);
		slot_symbols[symbol] = slot_symbol;
		return slot_symbol;
	}
};

// Unwind tables, notes, and comments contribute no items; .eh_frame
// is instead parsed back into typed host-EH facts (below) when it
// matches our own emission fingerprint.
bool SectionContributes(const ElfSection & section)
{
	if ((section.flags & kFlagAlloc) == 0)
		return false;
	if (section.type != kSectionProgbits && section.type != kSectionNobits)
		return false;
	if (section.name == ".eh_frame" || section.name == ".eh_frame_hdr")
		return false;
	if (section.name.compare(0, 5, ".note") == 0)
		return false;
	return true;
}

// One raw relocation, independent of the item/patch conversion.
struct RawReloc
{
	size_t symbol = 0;
	unsigned kind = 0;
	long long addend = 0;
};

// offset -> relocation for one target section.
map<unsigned long long, RawReloc> CollectSectionRelocs(
	const ElfFile & elf, const vector<ElfSection> & sections,
	unsigned target)
{
	map<unsigned long long, RawReloc> out;
	for (unsigned i = 0; i < sections.size(); i++)
	{
		if (sections[i].type != kSectionRela ||
		    sections[i].info != target || sections[i].entsize < 24)
			continue;
		size_t count =
			static_cast<size_t>(sections[i].size / sections[i].entsize);
		for (size_t r = 0; r < count; r++)
		{
			unsigned long long at =
				sections[i].offset + r * sections[i].entsize;
			RawReloc reloc;
			unsigned long long info = elf.field(at + 8, 8);
			reloc.symbol = static_cast<size_t>(info >> 32);
			reloc.kind = static_cast<unsigned>(info & 0xFFFFFFFF);
			reloc.addend = static_cast<long long>(elf.field(at + 16, 8));
			out[elf.field(at, 8)] = reloc;
		}
	}
	return out;
}

unsigned FindSectionByName(const vector<ElfSection> & sections,
                           const string & name)
{
	for (unsigned i = 0; i < sections.size(); i++)
		if (sections[i].name == name)
			return i;
	return 0;
}

// Standard role recovery: the entry is the defined `main`; init/fini
// hooks are the .init_array/.fini_array entries. Array targets
// relocated through a section symbol get a synthesized internal
// symbol at the resolved location.
void RecoverProgramRoles(ObjectModule & module, const ElfFile & elf,
                         const vector<ElfSection> & sections,
                         const vector<int> & section_items)
{
	for (size_t s = 0; s < module.symbols.size(); s++)
	{
		const ObjectSymbol & symbol = module.symbols[s];
		if (symbol.item >= 0 && symbol.external_name == "main" &&
		    (symbol.binding == ObjectSymbol::SB_STRONG ||
		     symbol.binding == ObjectSymbol::SB_WEAK))
		{
			module.entry_symbol = static_cast<int>(s);
			break;
		}
	}

	const char * const names[2] = { ".init_array", ".fini_array" };
	int * roles[2] = { &module.init_symbol, &module.fini_symbol };
	for (int r = 0; r < 2; r++)
	{
		unsigned index = FindSectionByName(sections, names[r]);
		if (index == 0)
			continue;
		map<unsigned long long, RawReloc> relocs =
			CollectSectionRelocs(elf, sections, index);
		if (relocs.empty())
			continue;
		const RawReloc & reloc = relocs.begin()->second;
		if (reloc.symbol >= module.symbols.size())
			continue;
		const ObjectSymbol & target = module.symbols[reloc.symbol];
		if (target.item < 0)
			continue;
		long long offset = target.offset + reloc.addend;
		int found = -1;
		for (size_t s = 0; s < module.symbols.size() && found < 0; s++)
			if (module.symbols[s].item == target.item &&
			    module.symbols[s].offset == offset &&
			    !module.symbols[s].low_name.empty())
				found = static_cast<int>(s);
		if (found < 0)
		{
			ObjectSymbol hook;
			hook.low_name = "__cppgm_role_hook";
			hook.binding = ObjectSymbol::SB_INTERNAL;
			hook.item = target.item;
			hook.offset = offset;
			module.symbols.push_back(hook);
			found = static_cast<int>(module.symbols.size()) - 1;
		}
		*roles[r] = found;
	}
}

// One parsed CIE: whether it carries our landing-pad profile (zPLR
// with a direct pcrel-sdata4 personality; host compilers use the
// indirect 0x9b form, which the private toolchain does not model).
struct CieProfile
{
	bool has_lsda = false;
	bool ours = false;
};

CieProfile ParseCieProfile(const vector<unsigned char> & frame,
                           size_t at, size_t end)
{
	CieProfile profile;
	size_t pos = at + 9;   // length, id, version
	string augmentation;
	while (pos < end && frame[pos] != 0)
		augmentation += static_cast<char>(frame[pos++]);
	pos++;
	if (augmentation.empty() || augmentation[0] != 'z')
		return profile;
	// code alignment, data alignment, return address column
	while (pos < end && (frame[pos] & 0x80))
		pos++;
	pos++;
	while (pos < end && (frame[pos] & 0x80))
		pos++;
	pos++;
	while (pos < end && (frame[pos] & 0x80))
		pos++;
	pos++;
	while (pos < end && (frame[pos] & 0x80))
		pos++;
	pos++;   // augmentation data length
	profile.has_lsda = augmentation.find('L') != string::npos;
	if (augmentation.find('P') != string::npos)
	{
		if (pos >= end)
			return profile;
		profile.ours = profile.has_lsda && frame[pos] == 0x1b;
	}
	return profile;
}

// Parses .eh_frame + .gcc_except_table back into typed host-EH facts
// for FDEs matching our emission profile. Foreign unwind data (host
// helper objects) is skipped, exactly like the pre-PA31 reader.
void RecoverEhFacts(ObjectModule & module, const ElfFile & elf,
                    const vector<ElfSection> & sections,
                    const vector<int> & section_items)
{
	unsigned frame_index = FindSectionByName(sections, ".eh_frame");
	unsigned except_index =
		FindSectionByName(sections, ".gcc_except_table");
	if (frame_index == 0 || except_index == 0)
		return;
	int except_item = section_items[except_index];
	if (except_item < 0)
		return;
	const ElfSection & frame_section = sections[frame_index];
	if (frame_section.offset + frame_section.size > elf.bytes.size())
		return;
	vector<unsigned char> frame(
		elf.bytes.begin() + static_cast<long>(frame_section.offset),
		elf.bytes.begin() +
			static_cast<long>(frame_section.offset +
			                  frame_section.size));
	map<unsigned long long, RawReloc> frame_relocs =
		CollectSectionRelocs(elf, sections, frame_index);
	map<unsigned long long, RawReloc> except_relocs =
		CollectSectionRelocs(elf, sections, except_index);
	const vector<unsigned char> & except_bytes =
		module.items[static_cast<size_t>(except_item)].bytes;

	map<size_t, CieProfile> cies;
	size_t pos = 0;
	while (pos + 8 <= frame.size())
	{
		unsigned long long length = 0;
		for (int b = 0; b < 4; b++)
			length |= static_cast<unsigned long long>(frame[pos + b])
				<< (8 * b);
		if (length == 0 || pos + 4 + length > frame.size())
			break;
		size_t end = pos + 4 + static_cast<size_t>(length);
		unsigned long long id = 0;
		for (int b = 0; b < 4; b++)
			id |= static_cast<unsigned long long>(frame[pos + 4 + b])
				<< (8 * b);
		if (id == 0)
		{
			cies[pos] = ParseCieProfile(frame, pos, end);
			pos = end;
			continue;
		}
		size_t cie_at = pos + 4 - static_cast<size_t>(id);
		map<size_t, CieProfile>::const_iterator cie = cies.find(cie_at);
		if (cie == cies.end() || !cie->second.ours)
		{
			pos = end;
			continue;
		}

		// our FDE shape: pc begin reloc at +8, range at +12,
		// augmentation length (1 byte, value 4), LSDA reloc at +17
		map<unsigned long long, RawReloc>::const_iterator begin =
			frame_relocs.find(pos + 8);
		map<unsigned long long, RawReloc>::const_iterator lsda =
			frame_relocs.find(pos + 17);
		if (begin == frame_relocs.end() || lsda == frame_relocs.end() ||
		    begin->second.symbol >= module.symbols.size() ||
		    lsda->second.symbol >= module.symbols.size() ||
		    pos + 17 + 4 > frame.size() || frame[pos + 16] != 4)
		{
			pos = end;
			continue;
		}
		const ObjectSymbol & code =
			module.symbols[begin->second.symbol];
		const ObjectSymbol & table =
			module.symbols[lsda->second.symbol];
		if (code.item < 0 || table.item != except_item)
		{
			pos = end;
			continue;
		}

		EhFunctionInfo eh;
		eh.item = code.item;
		eh.item_offset = static_cast<unsigned long long>(
			code.offset + begin->second.addend);
		unsigned long long range = 0;
		for (int b = 0; b < 4; b++)
			range |= static_cast<unsigned long long>(frame[pos + 12 + b])
				<< (8 * b);
		eh.code_size = range;
		unsigned long long lsda_offset = static_cast<unsigned long long>(
			table.offset + lsda->second.addend);
		vector<EhTypeRef> type_refs;
		if (!DecodeEhTable(except_bytes, lsda_offset, eh, type_refs))
		{
			pos = end;
			continue;
		}

		// ttype entries: pcrel to an 8-byte slot holding the abs64
		// _ZTI address; a relocation-less entry is a catch-all.
		map<long long, int> filter_symbol;
		for (size_t t = 0; t < type_refs.size(); t++)
		{
			map<unsigned long long, RawReloc>::const_iterator entry =
				except_relocs.find(lsda_offset +
				                   type_refs[t].lsda_offset);
			if (entry == except_relocs.end())
				continue;
			if (entry->second.symbol >= module.symbols.size())
				continue;
			const ObjectSymbol & slot_base =
				module.symbols[entry->second.symbol];
			if (slot_base.item < 0)
				continue;
			const ImageItem & slot_item =
				module.items[static_cast<size_t>(slot_base.item)];
			unsigned long long slot_offset =
				static_cast<unsigned long long>(slot_base.offset +
				                                entry->second.addend);
			for (size_t p = 0; p < slot_item.patches.size(); p++)
			{
				const X86Patch & patch = slot_item.patches[p];
				if (patch.offset == slot_offset &&
				    patch.kind == X86_PATCH_ABS && patch.size == 8 &&
				    patch.imm.has_label)
				{
					filter_symbol[type_refs[t].value] = patch.imm.label;
					break;
				}
			}
		}
		for (size_t c = 0; c < eh.chains.size(); c++)
		{
			for (size_t a = 0; a < eh.chains[c].size(); a++)
			{
				EhAction & action = eh.chains[c][a];
				if (action.kind != EhAction::EH_CATCH)
					continue;
				map<long long, int>::const_iterator known =
					filter_symbol.find(action.filter);
				if (known == filter_symbol.end())
					action.kind = EhAction::EH_CATCH_ALL;
				else
					action.type_symbol = known->second;
			}
		}
		module.eh_functions.push_back(eh);
		pos = end;
	}
}

}  // namespace

ObjectModule ParseElfObjectBytes(const string & bytes,
                                 const string & input_name,
                                 const string & target)
{
	ElfFile elf(bytes, input_name);
	if (!IsElfObjectBytes(bytes))
		elf.fail("missing ELF magic");
	if (elf.field(4, 1) != 2 || elf.field(5, 1) != 1)
		elf.fail("not little-endian ELF64");
	if (elf.field(0x10, 2) != kElfTypeRelocatable)
		elf.fail("not a relocatable object");
	if (elf.field(0x12, 2) != kElfMachineX86_64)
		elf.fail("not an x86-64 object");

	unsigned long long section_offset = elf.field(0x28, 8);
	unsigned entry_size = static_cast<unsigned>(elf.field(0x3A, 2));
	unsigned section_count = static_cast<unsigned>(elf.field(0x3C, 2));
	unsigned name_section = static_cast<unsigned>(elf.field(0x3E, 2));
	if (entry_size < 0x40)
		elf.fail("bad section entry size");

	vector<ElfSection> sections(section_count);
	for (unsigned i = 0; i < section_count; i++)
	{
		unsigned long long at = section_offset + i * entry_size;
		sections[i].type = static_cast<unsigned>(elf.field(at + 4, 4));
		sections[i].flags = elf.field(at + 8, 8);
		sections[i].offset = elf.field(at + 0x18, 8);
		sections[i].size = elf.field(at + 0x20, 8);
		sections[i].link = static_cast<unsigned>(elf.field(at + 0x28, 4));
		sections[i].info = static_cast<unsigned>(elf.field(at + 0x2C, 4));
		sections[i].addralign = elf.field(at + 0x30, 8);
		sections[i].entsize = elf.field(at + 0x38, 8);
	}
	if (name_section >= section_count)
		elf.fail("bad section name table index");
	for (unsigned i = 0; i < section_count; i++)
	{
		unsigned long long name_at =
			sections[name_section].offset +
			elf.field(section_offset + i * entry_size, 4);
		string name;
		while (name_at < bytes.size() && bytes[name_at] != '\0')
			name += bytes[name_at++];
		sections[i].name = name;
	}

	ObjectModule module;
	module.target = target;

	// Allocatable sections -> items.
	vector<int> section_items(section_count, -1);
	for (unsigned i = 0; i < section_count; i++)
	{
		if (!SectionContributes(sections[i]))
			continue;
		ImageItem item;
		if ((sections[i].addralign & (sections[i].addralign - 1)) != 0 ||
		    sections[i].addralign > 4096)
			elf.fail("bad section alignment");
		item.align = sections[i].addralign > 1
			? static_cast<size_t>(sections[i].addralign)
			: 1;
		item.is_code = (sections[i].flags & kFlagExecinstr) != 0;
		if (sections[i].type == kSectionNobits)
			item.bytes.assign(static_cast<size_t>(sections[i].size), 0);
		else
		{
			if (sections[i].offset + sections[i].size > bytes.size())
				elf.fail("section data out of range");
			const unsigned char * begin =
				reinterpret_cast<const unsigned char *>(
					bytes.data() + sections[i].offset);
			item.bytes.assign(begin,
			                  begin + static_cast<size_t>(
				                  sections[i].size));
		}
		module.items.push_back(item);
		section_items[i] = static_cast<int>(module.items.size() - 1);
	}

	// The symbol table (module symbol index == ELF symbol index).
	unsigned symtab_index = 0;
	for (unsigned i = 0; i < section_count; i++)
		if (sections[i].type == kSectionSymtab)
			symtab_index = i;
	if (symtab_index == 0)
		elf.fail("missing symbol table");
	const ElfSection & symtab = sections[symtab_index];
	if (symtab.entsize < 24 || symtab.link >= section_count ||
	    sections[symtab.link].type != kSectionStrtab)
		elf.fail("bad symbol table");
	unsigned long long strtab_at = sections[symtab.link].offset;
	size_t symbol_count =
		static_cast<size_t>(symtab.size / symtab.entsize);
	for (size_t s = 0; s < symbol_count; s++)
	{
		unsigned long long at = symtab.offset + s * symtab.entsize;
		unsigned long long name_at = strtab_at + elf.field(at, 4);
		string name;
		while (name_at < bytes.size() && bytes[name_at] != '\0')
			name += bytes[name_at++];
		unsigned info = static_cast<unsigned>(elf.field(at + 4, 1));
		unsigned shndx = static_cast<unsigned>(elf.field(at + 6, 2));
		unsigned long long value = elf.field(at + 8, 8);
		unsigned long long size = elf.field(at + 16, 8);
		unsigned binding = info >> 4;
		unsigned type = info & 0xF;

		ObjectSymbol symbol;
		symbol.low_name = name;
		if (shndx == kSectionIndexUndefined)
		{
			symbol.binding = s == 0 ? ObjectSymbol::SB_INTERNAL
			                        : ObjectSymbol::SB_UNDEFINED;
			symbol.external_name = name;
		}
		else if (shndx == kSectionIndexCommon)
		{
			// Tentative C definition: allocate zero storage here.
			ImageItem item;
			if ((value & (value - 1)) != 0 || value > 4096)
				elf.fail("bad common symbol alignment");
			item.align = value > 1 ? static_cast<size_t>(value) : 1;
			item.bytes.assign(static_cast<size_t>(size), 0);
			module.items.push_back(item);
			symbol.binding = ObjectSymbol::SB_STRONG;
			symbol.external_name = name;
			symbol.item = static_cast<int>(module.items.size() - 1);
		}
		else if (shndx == kSectionIndexAbsolute || shndx >= section_count)
		{
			symbol.binding = ObjectSymbol::SB_INTERNAL;
		}
		else
		{
			symbol.item = section_items[shndx];
			symbol.offset = static_cast<long long>(value);
			if (type == kSymbolTypeSection ||
			    binding == kSymbolBindingLocal)
			{
				symbol.binding = symbol.item >= 0
					? ObjectSymbol::SB_INTERNAL
					: ObjectSymbol::SB_UNDEFINED;
				if (symbol.item < 0)
					symbol.external_name = name;
			}
			else
			{
				symbol.binding = binding == kSymbolBindingWeak
					? ObjectSymbol::SB_WEAK
					: ObjectSymbol::SB_STRONG;
				symbol.external_name = name;
			}
		}
		module.symbols.push_back(symbol);
	}

	// Relocations against contributing sections -> patches.
	GotBuilder got;
	for (unsigned i = 0; i < section_count; i++)
	{
		if (sections[i].type != kSectionRela)
			continue;
		if (sections[i].info >= section_count)
			elf.fail("bad relocation target section");
		int item_index = section_items[sections[i].info];
		if (item_index < 0)
			continue;  // relocations of a dropped section
		if (sections[i].entsize < 24)
			elf.fail("bad relocation entry size");
		size_t count =
			static_cast<size_t>(sections[i].size / sections[i].entsize);
		for (size_t r = 0; r < count; r++)
		{
			unsigned long long at =
				sections[i].offset + r * sections[i].entsize;
			unsigned long long place = elf.field(at, 8);
			unsigned long long info = elf.field(at + 8, 8);
			long long addend = static_cast<long long>(elf.field(at + 16, 8));
			size_t symbol = static_cast<size_t>(info >> 32);
			unsigned kind = static_cast<unsigned>(info & 0xFFFFFFFF);
			if (symbol >= module.symbols.size())
				elf.fail("relocation references unknown symbol");

			X86Patch patch;
			patch.offset = static_cast<size_t>(place);
			int label = static_cast<int>(symbol);
			switch (kind)
			{
			case kRelocPc32:
			case kRelocPlt32:
				patch.size = 4;
				patch.kind = X86_PATCH_PCREL;
				patch.imm = X86Imm::Label(
					label,
					static_cast<unsigned long long>(addend + 4));
				break;
			case kRelocPc64:
				patch.size = 8;
				patch.kind = X86_PATCH_PCREL;
				patch.imm = X86Imm::Label(
					label,
					static_cast<unsigned long long>(addend + 8));
				break;
			case kRelocAbs64:
				patch.size = 8;
				patch.kind = X86_PATCH_ABS;
				patch.imm = X86Imm::Label(
					label, static_cast<unsigned long long>(addend));
				break;
			case kRelocAbs32Signed:
				patch.size = 4;
				patch.kind = X86_PATCH_ABS;
				patch.imm = X86Imm::Label(
					label, static_cast<unsigned long long>(addend));
				break;
			case kRelocAbs32:
				patch.size = 4;
				patch.kind = X86_PATCH_TRUNC;
				patch.imm = X86Imm::Label(
					label, static_cast<unsigned long long>(addend));
				break;
			case kRelocGotPcrel:
			case kRelocGotPcrelx:
			case kRelocRexGotPcrelx:
				patch.size = 4;
				patch.kind = X86_PATCH_PCREL;
				patch.imm = X86Imm::Label(
					got.SlotSymbol(module, symbol),
					static_cast<unsigned long long>(addend + 4));
				break;
			default:
				elf.fail("unsupported relocation kind in " +
				         sections[i].name);
			}
			ImageItem & item =
				module.items[static_cast<size_t>(item_index)];
			if (patch.offset + static_cast<size_t>(patch.size) >
			    item.bytes.size())
				elf.fail("relocation outside section data");
			item.patches.push_back(patch);
		}
	}

	RecoverProgramRoles(module, elf, sections, section_items);
	RecoverEhFacts(module, elf, sections, section_items);
	return module;
}

}  // namespace toolchain
