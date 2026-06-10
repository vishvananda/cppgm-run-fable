#include "x86/elf_program.h"

#include <fstream>
#include <stdexcept>

#include <sys/stat.h>

using std::ios;
using std::ofstream;
using std::runtime_error;

namespace {

const unsigned long long kImageBase = 0x400000;
const size_t kElfHeaderSize = 64;
const size_t kProgramHeaderSize = 56;
const size_t kHeaderBytes = kElfHeaderSize + kProgramHeaderSize;

void PutBytes(vector<unsigned char>& out, unsigned long long value,
              int size)
{
	for (int i = 0; i < size; i++)
		out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}

// ELF header plus one RWX PT_LOAD program header mapping the whole
// file (headers included) at kImageBase, as per Reading Assignment B.
void BuildElfHeaders(unsigned long long entry, size_t file_size,
                     vector<unsigned char>& out)
{
	const unsigned char ident[16] =
	{
		0x7F, 'E', 'L', 'F',
		2,  // 64-bit
		1,  // little-endian
		1,  // ELF version 1
		0,  // System V ABI
		0, 0, 0, 0, 0, 0, 0, 0
	};
	out.insert(out.end(), ident, ident + 16);
	PutBytes(out, 2, 2);       // e_type: executable
	PutBytes(out, 0x3E, 2);    // e_machine: x86-64
	PutBytes(out, 1, 4);       // e_version
	PutBytes(out, entry, 8);   // e_entry
	PutBytes(out, kElfHeaderSize, 8);  // e_phoff
	PutBytes(out, 0, 8);       // e_shoff: no sections
	PutBytes(out, 0, 4);       // e_flags
	PutBytes(out, kElfHeaderSize, 2);      // e_ehsize
	PutBytes(out, kProgramHeaderSize, 2);  // e_phentsize
	PutBytes(out, 1, 2);       // e_phnum
	PutBytes(out, 0, 2);       // e_shentsize
	PutBytes(out, 0, 2);       // e_shnum
	PutBytes(out, 0, 2);       // e_shstrndx

	PutBytes(out, 1, 4);       // p_type: PT_LOAD
	PutBytes(out, 7, 4);       // p_flags: read | write | execute
	PutBytes(out, 0, 8);       // p_offset
	PutBytes(out, kImageBase, 8);  // p_vaddr
	PutBytes(out, 0, 8);       // p_paddr
	PutBytes(out, file_size, 8);   // p_filesz
	PutBytes(out, file_size, 8);   // p_memsz
	PutBytes(out, 0x1000, 8);  // p_align
}

}  // namespace

ProgramImage::ProgramImage() : entry_label_(-1)
{
}

void ProgramImage::SetLabelCount(int count)
{
	label_item_.assign(static_cast<size_t>(count), 0);
}

void ProgramImage::AddItem(const ImageItem& item)
{
	items_.push_back(item);
}

void ProgramImage::BindLabel(int label, size_t item_index)
{
	label_item_[static_cast<size_t>(label)] = item_index;
}

void ProgramImage::SetEntryLabel(int label)
{
	entry_label_ = label;
}

void ProgramImage::Layout(vector<unsigned char>& payload,
                          unsigned long long& entry) const
{
	const unsigned long long payload_base = kImageBase + kHeaderBytes;

	// Item addresses are determined purely by sizes and alignment, so
	// one pass assigns them and a second pass fills label patches.
	vector<size_t> item_offset(items_.size(), 0);
	for (size_t i = 0; i < items_.size(); i++)
	{
		unsigned long long vaddr = payload_base + payload.size();
		size_t pad = static_cast<size_t>(
			(items_[i].align - vaddr % items_[i].align) % items_[i].align);
		payload.insert(payload.end(), pad, 0);
		item_offset[i] = payload.size();
		payload.insert(payload.end(), items_[i].bytes.begin(),
		               items_[i].bytes.end());
	}

	vector<unsigned long long> label_value(label_item_.size(), 0);
	for (size_t i = 0; i < label_item_.size(); i++)
		label_value[i] = payload_base + item_offset[label_item_[i]];

	for (size_t i = 0; i < items_.size(); i++)
	{
		for (size_t p = 0; p < items_[i].patches.size(); p++)
		{
			const X86Patch& patch = items_[i].patches[p];
			unsigned long long value = patch.imm.addend;
			if (patch.imm.has_label)
				value += label_value[static_cast<size_t>(patch.imm.label)];
			size_t at = item_offset[i] + patch.offset;
			for (int b = 0; b < patch.size; b++)
				payload[at + b] =
					static_cast<unsigned char>(value >> (8 * b));
		}
	}

	if (entry_label_ >= 0)
		entry = label_value[static_cast<size_t>(entry_label_)];
	else if (!items_.empty())
		entry = payload_base + item_offset[0];
	else
		entry = payload_base;
}

void ProgramImage::WriteExecutable(const string& path)
{
	vector<unsigned char> payload;
	unsigned long long entry = 0;
	Layout(payload, entry);

	vector<unsigned char> headers;
	BuildElfHeaders(entry, kHeaderBytes + payload.size(), headers);

	ofstream out(path.c_str(), ios::binary | ios::trunc);
	if (!out)
		throw runtime_error("cannot create output file: " + path);
	out.write(reinterpret_cast<const char*>(headers.data()),
	          static_cast<std::streamsize>(headers.size()));
	if (!payload.empty())
		out.write(reinterpret_cast<const char*>(payload.data()),
		          static_cast<std::streamsize>(payload.size()));
	out.close();
	if (!out)
		throw runtime_error("cannot write output file: " + path);
	if (chmod(path.c_str(), 0755) != 0)
		throw runtime_error("cannot make output file executable: " + path);
}
