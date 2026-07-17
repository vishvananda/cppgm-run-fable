#include "toolchain/object_module.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "toolchain/elf_reader.h"

using std::runtime_error;
using std::string;

// Shared object-input plumbing: file reads, target normalization, and
// content classification. PA31 on, the only on-disk object format is
// the host ET_REL ELF relocatable (elf_object.cpp writes it,
// elf_reader.cpp parses it back).

namespace toolchain {

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

}  // namespace toolchain
