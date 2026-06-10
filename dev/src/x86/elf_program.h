#pragma once

#include <cstddef>
#include <string>
#include <vector>

using std::size_t;
using std::string;
using std::vector;

#include "x86/x86_encoding.h"

// PA9 program image: a flat sequence of items (one per CY86
// statement) laid out in order into one RWX PT_LOAD segment loaded at
// 0x400000, headers included. Labels bind to an item and resolve to
// the item's post-padding virtual address; recorded patches then
// receive (label value + addend) truncated to the patch width. Code
// encodings have value-independent lengths, so a single sequential
// layout pass is final.

// One statement's contribution to the image. Code items have align 1;
// data items pad to their natural alignment with zero bytes.
struct ImageItem
{
	ImageItem() : align(1) {}

	size_t align;
	vector<unsigned char> bytes;
	vector<X86Patch> patches;
};

class ProgramImage
{
public:
	ProgramImage();

	void SetLabelCount(int count);
	void AddItem(const ImageItem& item);
	void BindLabel(int label, size_t item_index);

	// Entry point: the bound label, or the first item when none is set.
	void SetEntryLabel(int label);

	// Lays out the items, resolves labels, applies patches, and writes
	// the ELF executable (mode 0755). Throws std::runtime_error on any
	// I/O failure.
	void WriteExecutable(const string& path);

private:
	void Layout(vector<unsigned char>& payload,
	            unsigned long long& entry) const;

	vector<ImageItem> items_;
	vector<size_t> label_item_;
	int entry_label_;
};
