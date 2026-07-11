#include "toolchain/elf_reader.h"

#include <map>
#include <stdexcept>
#include <vector>

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

// Unwind tables, notes, and comments carry no program semantics for
// the PA29 runtime model.
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
	return module;
}

}  // namespace toolchain
