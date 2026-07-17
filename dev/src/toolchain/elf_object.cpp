#include "toolchain/elf_object.h"

#include <cstdio>
#include <deque>
#include <map>
#include <stdexcept>
#include <vector>

#include "toolchain/eh_table.h"

using std::map;
using std::runtime_error;
using std::string;
using std::vector;

// Hand-rolled ELF64 emission mirroring the reader's layout constants;
// see elf_reader.cpp for the parsing side of the same contract.

namespace toolchain {

namespace {

const unsigned kSectionProgbits = 1;
const unsigned kSectionSymtab = 2;
const unsigned kSectionStrtab = 3;
const unsigned kSectionRela = 4;
const unsigned kSectionInitArray = 14;
const unsigned kSectionFiniArray = 15;
const unsigned kSectionGroup = 17;            // SHT_GROUP
const unsigned kSectionUnwind = 0x70000001;   // SHT_X86_64_UNWIND

const unsigned long long kFlagWrite = 1;
const unsigned long long kFlagAlloc = 2;
const unsigned long long kFlagExecinstr = 4;
const unsigned long long kFlagInfoLink = 0x40;
const unsigned long long kFlagGroup = 0x200;  // SHF_GROUP
const unsigned long long kFlagTls = 0x400;    // SHF_TLS

const unsigned kGroupComdat = 1;              // GRP_COMDAT

const unsigned kRelocAbs64 = 1;         // R_X86_64_64
const unsigned kRelocPc32 = 2;          // R_X86_64_PC32
const unsigned kRelocPlt32 = 4;         // R_X86_64_PLT32
const unsigned kRelocAbs32 = 10;        // R_X86_64_32
const unsigned kRelocAbs32Signed = 11;  // R_X86_64_32S
const unsigned kRelocPc64 = 24;         // R_X86_64_PC64
const unsigned kRelocTpoff32 = 23;      // R_X86_64_TPOFF32

struct ElfReloc
{
	unsigned long long offset = 0;
	unsigned kind = 0;
	int symbol = 0;        // final ELF symtab index
	long long addend = 0;
};

struct ElfSymbolEntry
{
	string name;
	unsigned char info = 0;
	unsigned short shndx = 0;
	unsigned long long value = 0;
	unsigned long long size = 0;
};

// One output section under construction.
struct SectionData
{
	string name;
	unsigned type = kSectionProgbits;
	unsigned long long flags = 0;
	unsigned long long align = 1;
	unsigned long long entsize = 0;
	vector<unsigned char> bytes;
	vector<ElfReloc> relocs;
	bool present = false;
	int index = 0;          // section header index once present
	int symbol = 0;         // section symbol (ELF symtab index)
	// COMDAT member sections: the module symbol signing the group.
	int signature_symbol = -1;
	// The section's .rela header index, -1 when it carries no
	// relocations (group payloads list the member's .rela section).
	int rela_index = -1;
};

// One section header record under assembly (payload bound later for
// relocation sections).
struct SectionHeader
{
	string name;
	unsigned type = 0;
	unsigned long long flags = 0;
	unsigned long long offset = 0;
	unsigned long long size = 0;
	unsigned link = 0;
	unsigned info = 0;
	unsigned long long align = 1;
	unsigned long long entsize = 0;
	const vector<unsigned char> * payload = 0;
};

void AppendPadding(vector<unsigned char> & bytes, unsigned long long align)
{
	while (align > 1 && bytes.size() % align != 0)
		bytes.push_back(0);
}

void AppendWord(vector<unsigned char> & bytes, unsigned long long value,
                int size)
{
	for (int b = 0; b < size; b++)
		bytes.push_back(static_cast<unsigned char>(value >> (8 * b)));
}

// The writer state: item placement, symbol planning, section assembly,
// and the final file image.
class ElfObjectWriter
{
public:
	explicit ElfObjectWriter(const ObjectModule & module);

	void Write(const string & path);

private:
	void PlanSections();
	void PlaceItems();
	void FinalizeLayout();
	void PlanLocalSymbols();
	void PlanDefinedSymbols();
	int SymbolForModuleIndex(size_t module_symbol);
	int UndefinedByName(const string & name);
	int AppendGlobal(const ElfSymbolEntry & entry);
	void ConvertPatches();
	ElfReloc ConvertOnePatch(const X86Patch & patch);
	unsigned long long TypeSlotOffset(int type_symbol);
	void BuildEhSections();
	void AppendCie(bool with_personality);
	void AppendFde(size_t fn, bool with_lsda, unsigned long long lsda);
	void BuildRoleSections();
	void EmitSymbolRecord(const ElfSymbolEntry & entry);
	void EmitSymtab();
	void AppendGroupHeaders(vector<SectionHeader> & headers,
	                        vector<vector<unsigned char> > & group_payloads,
	                        size_t symtab_index);
	vector<SectionHeader> BuildSectionHeaders(
		vector<vector<unsigned char> > & rela_payloads,
		vector<vector<unsigned char> > & group_payloads,
		size_t & shstrtab_index);
	void EmitFile(const string & path);

	const ObjectModule & module_;
	SectionData text_, data_, tdata_, eh_frame_, except_, init_array_,
		fini_array_, note_;
	// COMDAT member sections, one per weak definition item (deque:
	// item placement holds stable pointers while appending).
	std::deque<SectionData> comdat_sections_;
	vector<SectionData *> layout_;      // present sections in order
	vector<SectionData *> item_section_;
	vector<unsigned long long> item_offset_;
	vector<int> defined_symbol_elf_;    // module symbol -> ELF index
	map<string, int> defined_by_name_;
	map<string, int> undefined_by_name_;
	map<int, unsigned long long> type_slots_;
	map<int, unsigned long long> function_size_;
	vector<ElfSymbolEntry> locals_, globals_;
	size_t global_base_ = 1;            // first global symtab index
	size_t cie_plr_offset_ = 0;
	vector<unsigned char> symtab_, strtab_, shstrtab_;
};

ElfObjectWriter::ElfObjectWriter(const ObjectModule & module)
	: module_(module)
{
	text_.name = ".text";
	text_.flags = kFlagAlloc | kFlagExecinstr;
	text_.align = 16;
	data_.name = ".data";
	data_.flags = kFlagAlloc | kFlagWrite;
	data_.align = 8;
	tdata_.name = ".tdata";
	tdata_.flags = kFlagAlloc | kFlagWrite | kFlagTls;
	tdata_.align = 8;
	eh_frame_.name = ".eh_frame";
	eh_frame_.type = kSectionUnwind;
	eh_frame_.flags = kFlagAlloc;
	eh_frame_.align = 8;
	except_.name = ".gcc_except_table";
	except_.flags = kFlagAlloc;
	except_.align = 4;
	init_array_.name = ".init_array";
	init_array_.type = kSectionInitArray;
	init_array_.flags = kFlagAlloc | kFlagWrite;
	init_array_.align = 8;
	init_array_.entsize = 8;
	fini_array_.name = ".fini_array";
	fini_array_.type = kSectionFiniArray;
	fini_array_.flags = kFlagAlloc | kFlagWrite;
	fini_array_.align = 8;
	fini_array_.entsize = 8;
	note_.name = ".note.GNU-stack";
}

// Section presence is a typed decision made up front: the fact
// surface distinguishes objects with and without unwind/LSDA data,
// so empty EH sections must not exist.
void ElfObjectWriter::PlanSections()
{
	text_.present = true;
	data_.present = true;
	for (size_t i = 0; i < module_.items.size(); i++)
		if (module_.items[i].is_thread_local)
			tdata_.present = true;
	eh_frame_.present = !module_.eh_functions.empty();
	for (size_t f = 0; f < module_.eh_functions.size(); f++)
		if (!module_.eh_functions[f].call_sites.empty())
			except_.present = true;
	init_array_.present = module_.init_symbol >= 0;
	fini_array_.present = module_.fini_symbol >= 0;
	note_.present = true;
}

// A weak definition item becomes its own COMDAT member section so the
// host linker coalesces duplicate inline/template emissions by group.
// An item also carrying a strong named definition stays in the plain
// section (COMDAT discard must never drop a strong definition).
void ElfObjectWriter::PlaceItems()
{
	item_section_.assign(module_.items.size(), 0);
	item_offset_.assign(module_.items.size(), 0);
	vector<int> comdat_symbol(module_.items.size(), -1);
	vector<bool> strong_named(module_.items.size(), false);
	for (size_t s = 0; s < module_.symbols.size(); s++)
	{
		const ObjectSymbol & symbol = module_.symbols[s];
		if (symbol.item < 0 || symbol.external_name.empty())
			continue;
		size_t item = static_cast<size_t>(symbol.item);
		if (symbol.binding == ObjectSymbol::SB_WEAK)
		{
			if (comdat_symbol[item] < 0)
				comdat_symbol[item] = static_cast<int>(s);
		}
		else
		{
			strong_named[item] = true;
		}
	}

	for (size_t i = 0; i < module_.items.size(); i++)
	{
		const ImageItem & item = module_.items[i];
		unsigned long long align = item.align > 1 ? item.align : 1;
		if ((align & (align - 1)) != 0 || align > 4096)
			throw runtime_error("bad item alignment in object emission");
		SectionData * section;
		if (item.is_thread_local)
		{
			section = &tdata_;
			if (align > section->align)
				section->align = align;
		}
		else if (comdat_symbol[i] >= 0 && !strong_named[i])
		{
			const string & name = module_
				.symbols[static_cast<size_t>(comdat_symbol[i])]
				.external_name;
			comdat_sections_.push_back(SectionData());
			SectionData & own = comdat_sections_.back();
			own.name = (item.is_code ? ".text." : ".data.") + name;
			own.flags = kFlagGroup |
				(item.is_code ? kFlagAlloc | kFlagExecinstr
				              : kFlagAlloc | kFlagWrite);
			own.align = align;
			own.present = true;
			own.signature_symbol = comdat_symbol[i];
			section = &own;
		}
		else
		{
			section = item.is_code ? &text_ : &data_;
			if (align > section->align)
				section->align = align;
		}
		AppendPadding(section->bytes, align);
		item_section_[i] = section;
		item_offset_[i] = section->bytes.size();
		section->bytes.insert(section->bytes.end(), item.bytes.begin(),
		                      item.bytes.end());
	}
	for (size_t f = 0; f < module_.eh_functions.size(); f++)
		function_size_[module_.eh_functions[f].item] =
			module_.eh_functions[f].code_size;
}

// Section header order: the SHT_GROUP headers occupy the first
// indices, then .text and its COMDAT members, .data and its COMDAT
// members, and the fixed tail sections.
void ElfObjectWriter::FinalizeLayout()
{
	layout_.push_back(&text_);
	for (std::deque<SectionData>::iterator c = comdat_sections_.begin();
	     c != comdat_sections_.end(); ++c)
		if (c->flags & kFlagExecinstr)
			layout_.push_back(&*c);
	layout_.push_back(&data_);
	for (std::deque<SectionData>::iterator c = comdat_sections_.begin();
	     c != comdat_sections_.end(); ++c)
		if (!(c->flags & kFlagExecinstr))
			layout_.push_back(&*c);
	SectionData * tail[6] = { &tdata_, &eh_frame_, &except_,
	                          &init_array_, &fini_array_, &note_ };
	for (int s = 0; s < 6; s++)
		if (tail[s]->present)
			layout_.push_back(tail[s]);

	size_t group_count = comdat_sections_.size();
	for (size_t s = 0; s < layout_.size(); s++)
		layout_[s]->index =
			static_cast<int>(group_count + s) + 1;
	for (size_t s = 0; s < layout_.size(); s++)
	{
		if (layout_[s] == &note_)
			continue;
		ElfSymbolEntry entry;
		entry.info = 3;   // LOCAL, SECTION
		entry.shndx = static_cast<unsigned short>(layout_[s]->index);
		locals_.push_back(entry);
		layout_[s]->symbol = static_cast<int>(locals_.size());
	}
}

// Internal-linkage definitions with an ABI spelling become named
// LOCAL symbols: host tools (nm, debuggers) see the entity while
// resolution still bypasses it entirely.
void ElfObjectWriter::PlanLocalSymbols()
{
	for (size_t s = 0; s < module_.symbols.size(); s++)
	{
		const ObjectSymbol & symbol = module_.symbols[s];
		if (symbol.item < 0 ||
		    symbol.binding != ObjectSymbol::SB_INTERNAL ||
		    symbol.local_name.empty())
			continue;
		const ImageItem & item =
			module_.items[static_cast<size_t>(symbol.item)];
		ElfSymbolEntry entry;
		entry.name = symbol.local_name;
		// LOCAL: TLS / FUNC / OBJECT
		entry.info = item.is_thread_local ? 6 : item.is_code ? 2 : 1;
		entry.shndx = static_cast<unsigned short>(
			item_section_[static_cast<size_t>(symbol.item)]->index);
		entry.value = item_offset_[static_cast<size_t>(symbol.item)] +
		              static_cast<unsigned long long>(symbol.offset);
		if (item.is_code && symbol.offset == 0 &&
		    function_size_.count(symbol.item))
			entry.size = function_size_[symbol.item];
		locals_.push_back(entry);
	}
	global_base_ = locals_.size() + 1;
}

int ElfObjectWriter::AppendGlobal(const ElfSymbolEntry & entry)
{
	globals_.push_back(entry);
	return static_cast<int>(global_base_ + globals_.size() - 1);
}

// Strong/weak definitions become global symbols. Defined names win
// the reference lookup so undefined module symbols naming a weak
// definition in the same object resolve to it.
void ElfObjectWriter::PlanDefinedSymbols()
{
	defined_symbol_elf_.assign(module_.symbols.size(), 0);
	for (size_t s = 0; s < module_.symbols.size(); s++)
	{
		const ObjectSymbol & symbol = module_.symbols[s];
		if (symbol.item < 0 && symbol.weak_undefined &&
		    !symbol.external_name.empty() &&
		    !undefined_by_name_.count(symbol.external_name))
		{
			// A weak undefined reference (the TLS init probe): the
			// host link resolves it to null when nothing defines it.
			ElfSymbolEntry entry;
			entry.name = symbol.external_name;
			entry.info = 0x20;   // WEAK, NOTYPE
			undefined_by_name_[symbol.external_name] =
				AppendGlobal(entry);
			continue;
		}
		if (symbol.item < 0 && symbol.thread_local_symbol &&
		    !symbol.external_name.empty() &&
		    !undefined_by_name_.count(symbol.external_name))
		{
			// An undefined thread_local reference carries STT_TLS so
			// the host linker accepts the TLS relocation against it.
			ElfSymbolEntry entry;
			entry.name = symbol.external_name;
			entry.info = 0x16;   // GLOBAL, TLS
			undefined_by_name_[symbol.external_name] =
				AppendGlobal(entry);
			continue;
		}
		if (symbol.item < 0 ||
		    symbol.binding == ObjectSymbol::SB_INTERNAL ||
		    symbol.external_name.empty())
			continue;
		const ImageItem & item =
			module_.items[static_cast<size_t>(symbol.item)];
		ElfSymbolEntry entry;
		entry.name = symbol.external_name;
		unsigned char bind = symbol.binding == ObjectSymbol::SB_WEAK
			? 2 : 1;
		// TLS / FUNC / OBJECT
		unsigned char type = item.is_thread_local ? 6
			: item.is_code ? 2 : 1;
		entry.info = static_cast<unsigned char>((bind << 4) | type);
		entry.shndx = static_cast<unsigned short>(
			item_section_[static_cast<size_t>(symbol.item)]->index);
		entry.value = item_offset_[static_cast<size_t>(symbol.item)] +
		              static_cast<unsigned long long>(symbol.offset);
		if (item.is_code && symbol.offset == 0 &&
		    function_size_.count(symbol.item))
			entry.size = function_size_[symbol.item];
		int index = AppendGlobal(entry);
		defined_symbol_elf_[s] = index;
		if (!defined_by_name_.count(symbol.external_name))
			defined_by_name_[symbol.external_name] = index;
	}
}

int ElfObjectWriter::UndefinedByName(const string & name)
{
	if (name.empty())
		throw runtime_error("undefined symbol without a name");
	map<string, int>::const_iterator known = undefined_by_name_.find(name);
	if (known != undefined_by_name_.end())
		return known->second;
	ElfSymbolEntry entry;
	entry.name = name;
	entry.info = 0x10;   // GLOBAL, NOTYPE
	int index = AppendGlobal(entry);
	undefined_by_name_[name] = index;
	return index;
}

int ElfObjectWriter::SymbolForModuleIndex(size_t module_symbol)
{
	const ObjectSymbol & symbol = module_.symbols[module_symbol];
	if (symbol.item >= 0 && defined_symbol_elf_[module_symbol] != 0)
		return defined_symbol_elf_[module_symbol];
	if (symbol.item >= 0)
		throw runtime_error(
			"internal symbol reached the named-symbol path");
	const string & name = symbol.external_name.empty()
		? symbol.low_name : symbol.external_name;
	map<string, int>::const_iterator defined =
		defined_by_name_.find(name);
	if (defined != defined_by_name_.end())
		return defined->second;
	return UndefinedByName(name);
}

// Internal definitions relocate through their section symbol; named
// definitions and undefined externals through global symbols.
ElfReloc ElfObjectWriter::ConvertOnePatch(const X86Patch & patch)
{
	ElfReloc reloc;
	long long addend = static_cast<long long>(patch.imm.addend);
	switch (patch.kind)
	{
	case X86_PATCH_PCREL:
		if (patch.size == 4)
		{
			reloc.kind = kRelocPlt32;
			addend -= 4;
		}
		else if (patch.size == 8)
		{
			reloc.kind = kRelocPc64;
			addend -= 8;
		}
		else
			throw runtime_error("unencodable pc-relative patch width");
		break;
	case X86_PATCH_ABS:
		reloc.kind = patch.size == 8 ? kRelocAbs64
			: patch.size == 4 ? kRelocAbs32Signed
			: 0;
		break;
	case X86_PATCH_TRUNC:
		reloc.kind = patch.size == 8 ? kRelocAbs64
			: patch.size == 4 ? kRelocAbs32
			: 0;
		break;
	case X86_PATCH_TPOFF:
		reloc.kind = patch.size == 4 ? kRelocTpoff32 : 0;
		break;
	}
	if (reloc.kind == 0)
		throw runtime_error("unencodable patch in object emission");

	size_t target = static_cast<size_t>(patch.imm.label);
	if (target >= module_.symbols.size())
		throw runtime_error("patch references unknown symbol");
	const ObjectSymbol & symbol = module_.symbols[target];
	if (symbol.item >= 0 &&
	    (symbol.binding == ObjectSymbol::SB_INTERNAL ||
	     symbol.external_name.empty()))
	{
		SectionData * section =
			item_section_[static_cast<size_t>(symbol.item)];
		reloc.symbol = section->symbol;
		addend += static_cast<long long>(
			item_offset_[static_cast<size_t>(symbol.item)]) +
			symbol.offset;
	}
	else
	{
		reloc.symbol = SymbolForModuleIndex(target);
	}
	reloc.addend = addend;
	return reloc;
}

void ElfObjectWriter::ConvertPatches()
{
	for (size_t i = 0; i < module_.items.size(); i++)
	{
		const ImageItem & item = module_.items[i];
		for (size_t p = 0; p < item.patches.size(); p++)
		{
			if (!item.patches[p].imm.has_label)
				continue;
			ElfReloc reloc = ConvertOnePatch(item.patches[p]);
			reloc.offset = item_offset_[i] + item.patches[p].offset;
			item_section_[i]->relocs.push_back(reloc);
		}
	}
}

// DW.ref-style indirection: one 8-byte .data slot per catch type,
// holding the absolute _ZTI address the LSDA type entries point at.
unsigned long long ElfObjectWriter::TypeSlotOffset(int type_symbol)
{
	map<int, unsigned long long>::const_iterator known =
		type_slots_.find(type_symbol);
	if (known != type_slots_.end())
		return known->second;
	AppendPadding(data_.bytes, 8);
	unsigned long long offset = data_.bytes.size();
	ElfReloc reloc;
	reloc.offset = offset;
	reloc.kind = kRelocAbs64;
	reloc.symbol = SymbolForModuleIndex(static_cast<size_t>(type_symbol));
	AppendWord(data_.bytes, 0, 8);
	data_.relocs.push_back(reloc);
	type_slots_[type_symbol] = offset;
	return offset;
}

void ElfObjectWriter::AppendCie(bool with_personality)
{
	vector<unsigned char> & out = eh_frame_.bytes;
	size_t start = out.size();
	AppendWord(out, 0, 4);   // length back-patched below
	AppendWord(out, 0, 4);   // CIE id
	out.push_back(1);        // version
	const char * augmentation = with_personality ? "zPLR" : "zR";
	for (const char * c = augmentation; *c; c++)
		out.push_back(static_cast<unsigned char>(*c));
	out.push_back(0);
	out.push_back(1);      // code alignment factor 1
	out.push_back(0x78);   // data alignment factor -8
	out.push_back(16);     // return address column (rip)
	if (with_personality)
	{
		out.push_back(7);      // augmentation data length
		out.push_back(0x1b);   // personality: pcrel sdata4, direct
		ElfReloc reloc;
		reloc.offset = out.size();
		reloc.kind = kRelocPc32;
		reloc.symbol = UndefinedByName("__gxx_personality_v0");
		eh_frame_.relocs.push_back(reloc);
		AppendWord(out, 0, 4);
		out.push_back(0x1b);   // LSDA encoding: pcrel sdata4
		out.push_back(0x1b);   // code encoding: pcrel sdata4
	}
	else
	{
		out.push_back(1);      // augmentation data length
		out.push_back(0x1b);   // code encoding: pcrel sdata4
	}
	// initial CFI: CFA = rsp + 8, return address at CFA - 8
	out.push_back(0x0c);
	out.push_back(7);
	out.push_back(8);
	out.push_back(0x80 | 16);
	out.push_back(1);
	while ((out.size() - start) % 8 != 0)
		out.push_back(0);   // DW_CFA_nop
	unsigned long long length = out.size() - start - 4;
	for (int b = 0; b < 4; b++)
		out[start + static_cast<size_t>(b)] =
			static_cast<unsigned char>(length >> (8 * b));
}

void ElfObjectWriter::AppendFde(size_t fn, bool with_lsda,
                                unsigned long long lsda)
{
	const EhFunctionInfo & eh = module_.eh_functions[fn];
	vector<unsigned char> & out = eh_frame_.bytes;
	size_t start = out.size();
	AppendWord(out, 0, 4);   // length back-patched below
	unsigned long long cie = with_lsda ? cie_plr_offset_ : 0;
	AppendWord(out, out.size() - static_cast<size_t>(cie), 4);
	{
		// pc begin: pcrel sdata4 against the function's own section
		// (COMDAT members included, so GNU linkers drop the FDE with
		// a discarded group).
		ElfReloc reloc;
		reloc.offset = out.size();
		reloc.kind = kRelocPc32;
		reloc.symbol =
			item_section_[static_cast<size_t>(eh.item)]->symbol;
		reloc.addend = static_cast<long long>(
			item_offset_[static_cast<size_t>(eh.item)] + eh.item_offset);
		eh_frame_.relocs.push_back(reloc);
	}
	AppendWord(out, 0, 4);
	AppendWord(out, eh.code_size, 4);
	if (with_lsda)
	{
		out.push_back(4);   // augmentation data: the LSDA pointer
		ElfReloc reloc;
		reloc.offset = out.size();
		reloc.kind = kRelocPc32;
		reloc.symbol = except_.symbol;
		reloc.addend = static_cast<long long>(lsda);
		eh_frame_.relocs.push_back(reloc);
		AppendWord(out, 0, 4);
	}
	else
	{
		out.push_back(0);
	}
	out.insert(out.end(), eh.cfi_ops.begin(), eh.cfi_ops.end());
	while ((out.size() - start) % 8 != 0)
		out.push_back(0);   // DW_CFA_nop
	unsigned long long length = out.size() - start - 4;
	for (int b = 0; b < 4; b++)
		out[start + static_cast<size_t>(b)] =
			static_cast<unsigned char>(length >> (8 * b));
}

void ElfObjectWriter::BuildEhSections()
{
	if (!eh_frame_.present)
		return;
	AppendCie(false);
	if (except_.present)
	{
		cie_plr_offset_ = eh_frame_.bytes.size();
		AppendCie(true);
	}
	for (size_t f = 0; f < module_.eh_functions.size(); f++)
	{
		const EhFunctionInfo & eh = module_.eh_functions[f];
		if (eh.call_sites.empty())
		{
			AppendFde(f, false, 0);
			continue;
		}
		EhTableBytes table = EncodeEhTable(eh);
		AppendPadding(except_.bytes, 4);
		unsigned long long lsda = except_.bytes.size();
		except_.bytes.insert(except_.bytes.end(), table.bytes.begin(),
		                     table.bytes.end());
		for (size_t t = 0; t < table.type_refs.size(); t++)
		{
			ElfReloc reloc;   // ttype entry -> indirection slot
			reloc.offset = lsda + table.type_refs[t].lsda_offset;
			reloc.kind = kRelocPc32;
			reloc.symbol = data_.symbol;
			reloc.addend = static_cast<long long>(
				TypeSlotOffset(table.type_refs[t].value));
			except_.relocs.push_back(reloc);
		}
		AppendFde(f, true, lsda);
	}
}

// Program roles in standard host form: init/fini hooks become
// .init_array/.fini_array entries (entry stays the global `main`).
void ElfObjectWriter::BuildRoleSections()
{
	const int roles[2] = { module_.init_symbol, module_.fini_symbol };
	SectionData * sections[2] = { &init_array_, &fini_array_ };
	for (int r = 0; r < 2; r++)
	{
		if (roles[r] < 0)
			continue;
		const ObjectSymbol & symbol =
			module_.symbols[static_cast<size_t>(roles[r])];
		if (symbol.item < 0)
			throw runtime_error("program role hook is undefined");
		ElfReloc reloc;
		reloc.offset = sections[r]->bytes.size();
		reloc.kind = kRelocAbs64;
		reloc.symbol =
			item_section_[static_cast<size_t>(symbol.item)]->symbol;
		reloc.addend = static_cast<long long>(
			item_offset_[static_cast<size_t>(symbol.item)]) +
			symbol.offset;
		sections[r]->relocs.push_back(reloc);
		AppendWord(sections[r]->bytes, 0, 8);
	}
}

void ElfObjectWriter::EmitSymbolRecord(const ElfSymbolEntry & entry)
{
	unsigned long long name_offset = 0;
	if (!entry.name.empty())
	{
		name_offset = strtab_.size();
		for (size_t c = 0; c < entry.name.size(); c++)
			strtab_.push_back(static_cast<unsigned char>(entry.name[c]));
		strtab_.push_back(0);
	}
	AppendWord(symtab_, name_offset, 4);
	symtab_.push_back(entry.info);
	symtab_.push_back(0);
	AppendWord(symtab_, entry.shndx, 2);
	AppendWord(symtab_, entry.value, 8);
	AppendWord(symtab_, entry.size, 8);
}

void ElfObjectWriter::EmitSymtab()
{
	strtab_.push_back(0);
	symtab_.assign(24, 0);   // the null symbol
	for (size_t l = 0; l < locals_.size(); l++)
		EmitSymbolRecord(locals_[l]);
	for (size_t g = 0; g < globals_.size(); g++)
		EmitSymbolRecord(globals_[g]);
}

// The SHT_GROUP headers: GRP_COMDAT, the member section, its .rela
// pair; sh_info names the signing symbol.
void ElfObjectWriter::AppendGroupHeaders(
	vector<SectionHeader> & headers,
	vector<vector<unsigned char> > & group_payloads,
	size_t symtab_index)
{
	for (std::deque<SectionData>::const_iterator c =
	         comdat_sections_.begin();
	     c != comdat_sections_.end(); ++c)
	{
		vector<unsigned char> payload;
		AppendWord(payload, kGroupComdat, 4);
		AppendWord(payload, static_cast<unsigned>(c->index), 4);
		if (c->rela_index >= 0)
			AppendWord(payload,
			           static_cast<unsigned>(c->rela_index), 4);
		group_payloads.push_back(payload);
	}
	size_t group = 0;
	for (std::deque<SectionData>::const_iterator c =
	         comdat_sections_.begin();
	     c != comdat_sections_.end(); ++c, ++group)
	{
		SectionHeader header;
		header.name = ".group";
		header.type = kSectionGroup;
		header.size = group_payloads[group].size();
		header.link = static_cast<unsigned>(symtab_index);
		header.info = static_cast<unsigned>(
			defined_symbol_elf_[static_cast<size_t>(
				c->signature_symbol)]);
		header.align = 4;
		header.entsize = 4;
		header.payload = &group_payloads[group];
		headers.push_back(header);
	}
}

// The section header table: null, the SHT_GROUP headers, the layout
// sections, their .rela pairs, then .symtab/.strtab/.shstrtab.
vector<SectionHeader> ElfObjectWriter::BuildSectionHeaders(
	vector<vector<unsigned char> > & rela_payloads,
	vector<vector<unsigned char> > & group_payloads,
	size_t & shstrtab_index)
{
	// Which layout sections carry relocations decides every later
	// index: .rela headers follow the layout block, .symtab follows
	// them.
	vector<size_t> rela_sources;
	for (size_t s = 0; s < layout_.size(); s++)
		if (!layout_[s]->relocs.empty())
			rela_sources.push_back(s);
	size_t rela_base = 1 + comdat_sections_.size() + layout_.size();
	size_t symtab_index = rela_base + rela_sources.size();
	for (size_t r = 0; r < rela_sources.size(); r++)
		layout_[rela_sources[r]]->rela_index =
			static_cast<int>(rela_base + r);

	vector<SectionHeader> headers;
	headers.push_back(SectionHeader());
	AppendGroupHeaders(headers, group_payloads, symtab_index);

	for (size_t s = 0; s < layout_.size(); s++)
	{
		const SectionData & section = *layout_[s];
		SectionHeader header;
		header.name = section.name;
		header.type = section.type;
		header.flags = section.flags;
		header.size = section.bytes.size();
		header.align = section.align;
		header.entsize = section.entsize;
		header.payload = &section.bytes;
		headers.push_back(header);
	}

	vector<size_t> rela_headers;
	for (size_t r = 0; r < rela_sources.size(); r++)
	{
		const SectionData & section = *layout_[rela_sources[r]];
		vector<unsigned char> payload;
		for (size_t i = 0; i < section.relocs.size(); i++)
		{
			const ElfReloc & reloc = section.relocs[i];
			AppendWord(payload, reloc.offset, 8);
			AppendWord(payload,
			           (static_cast<unsigned long long>(
				            static_cast<unsigned>(reloc.symbol)) << 32) |
			           reloc.kind, 8);
			AppendWord(payload,
			           static_cast<unsigned long long>(reloc.addend), 8);
		}
		rela_payloads.push_back(payload);
		SectionHeader header;
		header.name = ".rela" + section.name;
		header.type = kSectionRela;
		header.flags = kFlagInfoLink;
		if (section.flags & kFlagGroup)
			header.flags |= kFlagGroup;
		header.size = payload.size();
		header.info = static_cast<unsigned>(section.index);
		header.align = 8;
		header.entsize = 24;
		rela_headers.push_back(headers.size());
		headers.push_back(header);
	}
	for (size_t r = 0; r < rela_headers.size(); r++)
		headers[rela_headers[r]].payload = &rela_payloads[r];

	if (symtab_index != headers.size())
		throw runtime_error("section header planning drifted");
	{
		SectionHeader header;
		header.name = ".symtab";
		header.type = kSectionSymtab;
		header.size = symtab_.size();
		header.link = static_cast<unsigned>(symtab_index) + 1;
		header.info = static_cast<unsigned>(locals_.size()) + 1;
		header.align = 8;
		header.entsize = 24;
		header.payload = &symtab_;
		headers.push_back(header);
	}
	{
		SectionHeader header;
		header.name = ".strtab";
		header.type = kSectionStrtab;
		header.size = strtab_.size();
		header.payload = &strtab_;
		headers.push_back(header);
	}
	shstrtab_index = headers.size();
	{
		SectionHeader header;
		header.name = ".shstrtab";
		header.type = kSectionStrtab;
		header.payload = &shstrtab_;
		headers.push_back(header);
	}
	for (size_t r = 0; r < rela_headers.size(); r++)
		headers[rela_headers[r]].link =
			static_cast<unsigned>(symtab_index);
	return headers;
}

void ElfObjectWriter::EmitFile(const string & path)
{
	vector<vector<unsigned char> > rela_payloads;
	vector<vector<unsigned char> > group_payloads;
	size_t shstrtab_index = 0;
	vector<SectionHeader> headers =
		BuildSectionHeaders(rela_payloads, group_payloads,
		                    shstrtab_index);

	shstrtab_.push_back(0);
	vector<unsigned long long> name_offsets(headers.size(), 0);
	for (size_t h = 1; h < headers.size(); h++)
	{
		name_offsets[h] = shstrtab_.size();
		for (size_t c = 0; c < headers[h].name.size(); c++)
			shstrtab_.push_back(
				static_cast<unsigned char>(headers[h].name[c]));
		shstrtab_.push_back(0);
	}
	headers[shstrtab_index].size = shstrtab_.size();

	vector<unsigned char> file(64, 0);
	for (size_t h = 1; h < headers.size(); h++)
	{
		AppendPadding(file, headers[h].align > 1 ? headers[h].align : 1);
		headers[h].offset = file.size();
		if (headers[h].payload)
			file.insert(file.end(), headers[h].payload->begin(),
			            headers[h].payload->end());
	}
	AppendPadding(file, 8);
	unsigned long long shoff = file.size();
	for (size_t h = 0; h < headers.size(); h++)
	{
		AppendWord(file, name_offsets[h], 4);
		AppendWord(file, headers[h].type, 4);
		AppendWord(file, headers[h].flags, 8);
		AppendWord(file, 0, 8);   // sh_addr
		AppendWord(file, h == 0 ? 0 : headers[h].offset, 8);
		AppendWord(file, headers[h].size, 8);
		AppendWord(file, headers[h].link, 4);
		AppendWord(file, headers[h].info, 4);
		AppendWord(file, headers[h].align, 8);
		AppendWord(file, headers[h].entsize, 8);
	}

	file[0] = 0x7f;
	file[1] = 'E';
	file[2] = 'L';
	file[3] = 'F';
	file[4] = 2;    // ELFCLASS64
	file[5] = 1;    // little endian
	file[6] = 1;    // EV_CURRENT
	file[0x10] = 1;      // ET_REL
	file[0x12] = 0x3E;   // EM_X86_64
	file[0x14] = 1;      // e_version
	for (int b = 0; b < 8; b++)
		file[0x28 + b] = static_cast<unsigned char>(shoff >> (8 * b));
	file[0x34] = 64;   // e_ehsize
	file[0x3A] = 64;   // e_shentsize
	file[0x3C] = static_cast<unsigned char>(headers.size());
	file[0x3D] = static_cast<unsigned char>(headers.size() >> 8);
	file[0x3E] = static_cast<unsigned char>(shstrtab_index);
	file[0x3F] = static_cast<unsigned char>(shstrtab_index >> 8);

	FILE * out = fopen(path.c_str(), "wb");
	if (!out)
		throw runtime_error("cannot open output file: " + path);
	size_t written = fwrite(file.data(), 1, file.size(), out);
	if (fclose(out) != 0 || written != file.size())
		throw runtime_error("cannot write output file: " + path);
}

void ElfObjectWriter::Write(const string & path)
{
	PlanSections();
	PlaceItems();
	FinalizeLayout();
	PlanLocalSymbols();
	PlanDefinedSymbols();
	ConvertPatches();
	BuildEhSections();
	BuildRoleSections();
	EmitSymtab();
	EmitFile(path);
}

}  // namespace

void WriteElfObjectFile(const string & path, const ObjectModule & module)
{
	if (module.target != "linux")
		throw runtime_error(
			"host object emission supports the linux target only");
	ElfObjectWriter writer(module);
	writer.Write(path);
}

}  // namespace toolchain
