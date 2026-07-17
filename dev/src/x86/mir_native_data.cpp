#include "x86/mir_native_data.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

using std::logic_error;

namespace mir_native {

namespace {

typedef mir_model::MirGlobalDefinition Global;

std::size_t DataItemAlign(const Global::DataItem & item)
{
	switch (item.kind)
	{
	case Global::DataItem::ITEM_INTEGER:
	case Global::DataItem::ITEM_FLOAT:
		return ScalarSize(item.type);
	case Global::DataItem::ITEM_ADDR:
		return 8;
	case Global::DataItem::ITEM_ZERO:
		return 1;
	}
	throw logic_error("unknown global data item kind");
}

void AppendAddrPatch(ImageItem & item, ISymbolLabels & labels,
                     const std::string & symbol, long long addend)
{
	X86Patch patch;
	patch.offset = item.bytes.size();
	patch.size = 8;
	patch.kind = X86_PATCH_ABS;
	patch.imm = X86Imm::Label(labels.SymbolLabel(symbol),
	                          static_cast<unsigned long long>(addend));
	item.patches.push_back(patch);
	item.bytes.insert(item.bytes.end(), 8, 0);
}

void EncodeScalarGlobal(ISymbolLabels & labels, const Global & global,
                        ImageItem & item)
{
	std::size_t size = ScalarSize(global.type);
	item.align = size;
	switch (global.init_kind)
	{
	case Global::GI_ZERO:
		item.bytes.assign(size, 0);
		return;
	case Global::GI_INTEGER:
		AppendLittleEndian(
			item.bytes,
			static_cast<unsigned long long>(global.int_value), size);
		return;
	case Global::GI_FLOAT:
		AppendFloatBits(item.bytes, global.type,
		                ParseFloatLiteral(global.type, global.literal_text,
		                                  global.float_value));
		return;
	case Global::GI_ADDR:
		AppendAddrPatch(item, labels, global.symbol, global.addr_addend);
		return;
	}
	throw logic_error("unknown global init kind");
}

void EncodeDataGlobal(ISymbolLabels & labels, const Global & global,
                      ImageItem & item)
{
	std::size_t max_align = 8;
	// Every data item appends at most 16 bytes plus alignment padding;
	// zero rows carry their own count.
	std::size_t reserve = 0;
	for (std::size_t i = 0; i < global.data_items.size(); i++)
		reserve += 32 + global.data_items[i].zero_bytes;
	item.bytes.reserve(reserve);
	for (std::size_t i = 0; i < global.data_items.size(); i++)
	{
		const Global::DataItem & data = global.data_items[i];
		std::size_t align = DataItemAlign(data);
		if (align > max_align)
			max_align = align;
		while (item.bytes.size() % align != 0)
			item.bytes.push_back(0);
		switch (data.kind)
		{
		case Global::DataItem::ITEM_INTEGER:
			AppendLittleEndian(
				item.bytes,
				static_cast<unsigned long long>(data.int_value),
				ScalarSize(data.type));
			break;
		case Global::DataItem::ITEM_FLOAT:
			AppendFloatBits(item.bytes, data.type,
			                ParseFloatLiteral(data.type, data.literal_text,
			                                  data.float_value));
			break;
		case Global::DataItem::ITEM_ADDR:
			AppendAddrPatch(item, labels, data.symbol, data.addr_addend);
			break;
		case Global::DataItem::ITEM_ZERO:
			item.bytes.insert(item.bytes.end(), data.zero_bytes, 0);
			break;
		}
	}
	item.align = max_align;
}

}  // namespace

int TypeBits(const std::string & type)
{
	if (type == "i1" || type == "i8" || type == "u8")
		return 8;
	if (type == "i16" || type == "u16")
		return 16;
	if (type == "i32" || type == "u32" || type == "f32")
		return 32;
	if (type == "f80")
		return 80;
	if (type == "i128")
		return 128;
	// i64/u64/ptr/f64 and the untyped default are 64-bit wide; anything
	// else is a producer bug and must not degrade silently.
	if (type == "i64" || type == "u64" || type == "ptr" || type == "f64" ||
	    type.empty())
		return 64;
	throw logic_error("unknown machine IR type spelling: " + type);
}

std::size_t ScalarSize(const std::string & type)
{
	if (type == "f80")
		return 16;
	return static_cast<std::size_t>(TypeBits(type)) / 8;
}

void AppendLittleEndian(std::vector<unsigned char> & out,
                        unsigned long long value, std::size_t size)
{
	// Sizes past the 64-bit carrier (i128 scalars) sign-extend the
	// stored 64-bit value.
	unsigned char extension =
		(value >> 63) != 0 ? static_cast<unsigned char>(0xff) : 0;
	for (std::size_t i = 0; i < size; i++)
		out.push_back(i < 8
			? static_cast<unsigned char>(value >> (8 * i))
			: extension);
}

void AppendFloatBits(std::vector<unsigned char> & out,
                     const std::string & type, long double value)
{
	unsigned char raw[16];
	std::memset(raw, 0, sizeof(raw));
	if (type == "f32")
	{
		float narrow = static_cast<float>(value);
		std::memcpy(raw, &narrow, 4);
		out.insert(out.end(), raw, raw + 4);
	}
	else if (type == "f64")
	{
		double narrow = static_cast<double>(value);
		std::memcpy(raw, &narrow, 8);
		out.insert(out.end(), raw, raw + 8);
	}
	else
	{
		std::memcpy(raw, &value, 10);
		out.insert(out.end(), raw, raw + 16);
	}
}

long double ParseFloatLiteral(const std::string & type,
                              const std::string & text,
                              long double fallback)
{
	if (text.empty())
		return fallback;
	if (type == "f32")
		return strtof(text.c_str(), 0);
	if (type == "f64")
		return strtod(text.c_str(), 0);
	return strtold(text.c_str(), 0);
}

void EncodeGlobal(ISymbolLabels & labels,
                  const mir_model::MirGlobalDefinition & global,
                  ImageItem & item)
{
	if (global.storage_kind == mir_model::MirGlobalDefinition::GS_SCALAR)
		EncodeScalarGlobal(labels, global, item);
	else
		EncodeDataGlobal(labels, global, item);
}


namespace {

void AppendPatch(ImageItem & item, std::size_t offset, int size,
                 EX86PatchKind kind, int label)
{
	X86Patch patch;
	patch.offset = offset;
	patch.size = size;
	patch.kind = kind;
	patch.imm = X86Imm::Label(label, 0);
	item.patches.push_back(patch);
}

}  // namespace

// PA32 host TLS: the per-TU thread_local wrapper body. It runs the
// module's own guarded init when one exists, otherwise probes the
// weak _ZTH init hook, then returns the thread-pointer-relative
// address of the backing storage (local-exec R_X86_64_TPOFF32).
ImageItem EncodeTlsWrapperItem(int init_label, int probe_label,
                               int global_label)
{
	ImageItem item;
	item.is_code = true;
	item.align = 16;
	std::vector<unsigned char> & b = item.bytes;
	const unsigned char sub_rsp[4] = { 0x48, 0x83, 0xEC, 0x08 };
	b.insert(b.end(), sub_rsp, sub_rsp + 4);
	if (init_label >= 0)
	{
		b.push_back(0xE8);   // call rel32 <init>
		AppendPatch(item, b.size(), 4, X86_PATCH_PCREL, init_label);
		b.insert(b.end(), 4, 0);
	}
	else if (probe_label >= 0)
	{
		b.push_back(0x48);   // movabs rax, <probe>
		b.push_back(0xB8);
		AppendPatch(item, b.size(), 8, X86_PATCH_ABS, probe_label);
		b.insert(b.end(), 8, 0);
		const unsigned char test_call[7] = {
			0x48, 0x85, 0xC0,   // test rax, rax
			0x74, 0x02,         // je +2
			0xFF, 0xD0,         // call rax
		};
		b.insert(b.end(), test_call, test_call + 7);
	}
	const unsigned char fs_zero[9] = {
		0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
	};   // mov rax, fs:[0]
	b.insert(b.end(), fs_zero, fs_zero + 9);
	b.push_back(0x48);   // lea rax, [rax + <global>@tpoff]
	b.push_back(0x8D);
	b.push_back(0x80);
	AppendPatch(item, b.size(), 4, X86_PATCH_TPOFF, global_label);
	b.insert(b.end(), 4, 0);
	const unsigned char epilogue[5] = {
		0x48, 0x83, 0xC4, 0x08,   // add rsp, 8
		0xC3,                     // ret
	};
	b.insert(b.end(), epilogue, epilogue + 5);
	return item;
}

}  // namespace mir_native
