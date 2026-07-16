#include "toolchain/object_module.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "toolchain/elf_reader.h"

using std::runtime_error;
using std::string;

// Binary object encoding: a magic header, then length-prefixed fields
// in little-endian order. The format is versioned through the magic so
// later assignments can evolve it without misreading old files.

namespace toolchain {

namespace {

const char kObjectMagic[8] =
	{'C', 'P', 'G', 'M', 'O', 'B', 'J', '1'};

void AppendU64(string & out, unsigned long long value)
{
	for (int i = 0; i < 8; i++)
		out.push_back(static_cast<char>(value >> (8 * i)));
}

void AppendU32(string & out, unsigned long value)
{
	for (int i = 0; i < 4; i++)
		out.push_back(static_cast<char>(value >> (8 * i)));
}

void AppendI32(string & out, long value)
{
	AppendU32(out, static_cast<unsigned long>(value));
}

void AppendString(string & out, const string & value)
{
	AppendU32(out, value.size());
	out += value;
}

// Bounds-checked little-endian reads over the file bytes.
struct ObjectReader
{
	ObjectReader(const string & bytes, const string & input_name)
		: bytes(bytes), input_name(input_name), at(0)
	{
	}

	void fail(const string & what) const
	{
		throw runtime_error("bad compiler object file " + input_name +
		                    ": " + what);
	}

	void need(size_t count) const
	{
		if (at + count > bytes.size())
			fail("truncated file");
	}

	unsigned long long u64()
	{
		need(8);
		unsigned long long value = 0;
		for (int i = 0; i < 8; i++)
			value |= static_cast<unsigned long long>(
				static_cast<unsigned char>(bytes[at + i])) << (8 * i);
		at += 8;
		return value;
	}

	unsigned long u32()
	{
		need(4);
		unsigned long value = 0;
		for (int i = 0; i < 4; i++)
			value |= static_cast<unsigned long>(
				static_cast<unsigned char>(bytes[at + i])) << (8 * i);
		at += 4;
		return value;
	}

	long i32()
	{
		return static_cast<long>(static_cast<int>(u32()));
	}

	unsigned char u8()
	{
		need(1);
		return static_cast<unsigned char>(bytes[at++]);
	}

	string str()
	{
		unsigned long size = u32();
		need(size);
		string value = bytes.substr(at, size);
		at += size;
		return value;
	}

	const string & bytes;
	const string & input_name;
	size_t at;
};

}  // namespace

string NormalizeTargetName(const string & target)
{
	if (target.empty() || target == "linux" ||
	    target == "x86_64-unknown-linux-gnu" ||
	    target == "x86_64-pc-linux-gnu" || target == "x86_64-linux-gnu")
		return "linux";
	throw runtime_error("unsupported target: " + target);
}

string ReadFileBytes(const string & path)
{
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in)
		throw runtime_error("unable to read input file: " + path);
	std::ostringstream text;
	text << in.rdbuf();
	if (!in.good() && !in.eof())
		throw runtime_error("unable to read input file: " + path);
	return text.str();
}

bool IsElfObjectBytes(const string & bytes)
{
	return bytes.size() >= 4 && bytes[0] == '\x7f' && bytes[1] == 'E' &&
	       bytes[2] == 'L' && bytes[3] == 'F';
}

bool IsCppgmObjectBytes(const string & bytes)
{
	return bytes.size() >= sizeof(kObjectMagic) &&
	       bytes.compare(0, sizeof(kObjectMagic), kObjectMagic,
	                     sizeof(kObjectMagic)) == 0;
}

bool HasObjectFileName(const string & path)
{
	size_t dot = path.rfind('.');
	if (dot == string::npos)
		return false;
	string extension = path.substr(dot);
	return extension == ".o" || extension == ".obj";
}

ObjectModule LoadObjectModuleFile(const string & path,
                                  const string & target)
{
	const string bytes = ReadFileBytes(path);
	if (IsCppgmObjectBytes(bytes))
		return ParseObjectModuleBytes(bytes, path);
	if (IsElfObjectBytes(bytes))
		return ParseElfObjectBytes(bytes, path, target);
	throw runtime_error("unrecognized object file format: " + path);
}

string FindLibraryObject(const std::vector<string> & lib_dirs,
                         const string & name)
{
	static const char * const suffixes[] = {".o", ".obj"};
	for (size_t d = 0; d < lib_dirs.size(); d++)
	{
		string dir = lib_dirs[d];
		if (!dir.empty() && dir[dir.size() - 1] != '/')
			dir += '/';
		for (size_t s = 0; s < 2; s++)
		{
			const string candidate = dir + "lib" + name + suffixes[s];
			std::ifstream probe(candidate.c_str(),
			                    std::ios::in | std::ios::binary);
			if (probe)
				return candidate;
		}
	}
	throw runtime_error("cannot find library: -l" + name);
}

void WriteObjectModuleFile(const string & path, const ObjectModule & module)
{
	string out;
	out.append(kObjectMagic, sizeof(kObjectMagic));
	AppendString(out, module.target);

	AppendU32(out, module.symbols.size());
	for (size_t i = 0; i < module.symbols.size(); i++)
	{
		const ObjectSymbol & symbol = module.symbols[i];
		out.push_back(static_cast<char>(symbol.binding));
		AppendString(out, symbol.low_name);
		AppendString(out, symbol.external_name);
		AppendI32(out, symbol.item);
		AppendU64(out, static_cast<unsigned long long>(symbol.offset));
	}

	AppendU32(out, module.items.size());
	for (size_t i = 0; i < module.items.size(); i++)
	{
		const ImageItem & item = module.items[i];
		AppendU64(out, item.align);
		out.push_back(item.is_code ? 1 : 0);
		AppendU32(out, item.bytes.size());
		out.append(reinterpret_cast<const char *>(item.bytes.data()),
		           item.bytes.size());
		AppendU32(out, item.patches.size());
		for (size_t p = 0; p < item.patches.size(); p++)
		{
			const X86Patch & patch = item.patches[p];
			AppendU64(out, patch.offset);
			out.push_back(static_cast<char>(patch.size));
			out.push_back(static_cast<char>(patch.kind));
			out.push_back(patch.imm.has_label ? 1 : 0);
			AppendI32(out, patch.imm.label);
			AppendU64(out, patch.imm.addend);
		}
	}

	AppendI32(out, module.entry_symbol);
	AppendI32(out, module.init_symbol);
	AppendI32(out, module.fini_symbol);

	std::ofstream file(path.c_str(),
	                   std::ios::out | std::ios::binary | std::ios::trunc);
	if (!file)
		throw runtime_error("cannot create output file: " + path);
	file.write(out.data(), static_cast<std::streamsize>(out.size()));
	file.close();
	if (!file)
		throw runtime_error("cannot write output file: " + path);
}

ObjectModule ParseObjectModuleBytes(const string & bytes,
                                    const string & input_name)
{
	ObjectReader reader(bytes, input_name);
	if (!IsCppgmObjectBytes(bytes))
		reader.fail("missing object magic");
	reader.at = sizeof(kObjectMagic);

	ObjectModule module;
	module.target = reader.str();

	unsigned long symbol_count = reader.u32();
	for (unsigned long i = 0; i < symbol_count; i++)
	{
		ObjectSymbol symbol;
		unsigned char binding = reader.u8();
		if (binding > ObjectSymbol::SB_INTERNAL)
			reader.fail("bad symbol binding");
		symbol.binding = static_cast<ObjectSymbol::Binding>(binding);
		symbol.low_name = reader.str();
		symbol.external_name = reader.str();
		symbol.item = static_cast<int>(reader.i32());
		symbol.offset = static_cast<long long>(reader.u64());
		module.symbols.push_back(symbol);
	}

	unsigned long item_count = reader.u32();
	for (unsigned long i = 0; i < item_count; i++)
	{
		ImageItem item;
		item.align = static_cast<size_t>(reader.u64());
		if (item.align == 0 || (item.align & (item.align - 1)) != 0 ||
		    item.align > 4096)
			reader.fail("bad item alignment");
		item.is_code = reader.u8() != 0;
		unsigned long byte_count = reader.u32();
		reader.need(byte_count);
		item.bytes.assign(
			reinterpret_cast<const unsigned char *>(
				bytes.data() + reader.at),
			reinterpret_cast<const unsigned char *>(
				bytes.data() + reader.at + byte_count));
		reader.at += byte_count;
		unsigned long patch_count = reader.u32();
		for (unsigned long p = 0; p < patch_count; p++)
		{
			X86Patch patch;
			patch.offset = static_cast<size_t>(reader.u64());
			patch.size = reader.u8();
			unsigned char kind = reader.u8();
			if (kind > X86_PATCH_PCREL)
				reader.fail("bad patch kind");
			patch.kind = static_cast<EX86PatchKind>(kind);
			patch.imm.has_label = reader.u8() != 0;
			patch.imm.label = static_cast<int>(reader.i32());
			patch.imm.addend = reader.u64();
			if (patch.offset + static_cast<size_t>(patch.size) >
			    item.bytes.size())
				reader.fail("patch outside item bytes");
			if (patch.imm.has_label &&
			    (patch.imm.label < 0 ||
			     static_cast<unsigned long>(patch.imm.label) >=
			         symbol_count))
				reader.fail("patch references unknown symbol");
			item.patches.push_back(patch);
		}
		module.items.push_back(item);
	}

	module.entry_symbol = static_cast<int>(reader.i32());
	module.init_symbol = static_cast<int>(reader.i32());
	module.fini_symbol = static_cast<int>(reader.i32());
	int role_symbols[3] = {module.entry_symbol, module.init_symbol,
	                       module.fini_symbol};
	for (int i = 0; i < 3; i++)
		if (role_symbols[i] >= static_cast<int>(module.symbols.size()))
			reader.fail("role symbol out of range");

	for (size_t i = 0; i < module.symbols.size(); i++)
		if (module.symbols[i].item >=
		    static_cast<int>(module.items.size()))
			reader.fail("symbol references unknown item");
	return module;
}

}  // namespace toolchain
