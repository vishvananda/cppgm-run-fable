#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mir_model.h"
#include "x86/elf_program.h"

// Native data encoding shared by the instruction encoder and the
// global-image emission: MIR type widths, float storage images, and
// the encoding of one global definition into an image item. Symbol
// references resolve through the caller's label table.

namespace mir_native {

// MIR type spelling -> operand width in bits (f80 -> 80).
int TypeBits(const std::string & type);

// Storage bytes of a scalar of the named type (f80 stores as 16).
std::size_t ScalarSize(const std::string & type);

// Storage bytes of a value in the named float format: f32/f64 use
// their IEEE image, f80 the ten x87 bytes padded to sixteen.
void AppendFloatBits(std::vector<unsigned char> & out,
                     const std::string & type, long double value);

// Parse at the type's own width: rounding text through long double
// first can double-round (623e+100 lands one f64 ulp off strtod).
long double ParseFloatLiteral(const std::string & type,
                              const std::string & text,
                              long double fallback);

void AppendLittleEndian(std::vector<unsigned char> & out,
                        unsigned long long value, std::size_t size);

// The encoder's symbol-name -> image-label mapping.
struct ISymbolLabels
{
	virtual ~ISymbolLabels() {}
	virtual int SymbolLabel(const std::string & name) = 0;
};

// Encodes one global definition (scalar or structured data image)
// into an image item; address initializers become ABS patches.
void EncodeGlobal(ISymbolLabels & labels,
                  const mir_model::MirGlobalDefinition & global,
                  ImageItem & item);

// PA32 host TLS: the per-TU thread_local wrapper body. It runs the
// module's own guarded init when `init_label` >= 0, otherwise probes
// the weak init hook `probe_label` (when >= 0), then returns the
// thread-pointer-relative address of `global_label`'s storage
// (local-exec R_X86_64_TPOFF32).
ImageItem EncodeTlsWrapperItem(int init_label, int probe_label,
                               int global_label);

// One pooled data constant (float literals, sign masks).
struct PoolEntry
{
	std::vector<unsigned char> bytes;
	std::size_t align;
	int label;
};

// Program-wide names and the literal pool. Labels are dense image
// label ids: functions and globals first, pool constants (and, for
// relocatable modules, referenced external symbols) as they are
// discovered during function encoding.
class ProgramEnv : public ISymbolLabels
{
public:
	ProgramEnv(const mir_model::MirProgram & program, bool allow_external);

	const mir_model::MirProgram & program() const { return program_; }
	int SymbolLabel(const std::string & name);  // ISymbolLabels
	bool HasFunction(const std::string & name) const;
	const std::string & TlsBackingGlobal(const std::string & wrapper) const;

	// Label of a pooled constant holding `value` in the given float
	// format (32/64/80 bits), deduplicated by representation.
	int FloatConstantLabel(int bits, long double value);
	int ByteConstantLabel(const std::string & key,
	                      const unsigned char * data, std::size_t size,
	                      std::size_t align);

	int label_count() const { return next_label_; }
	const std::vector<PoolEntry> & pool() const { return pool_; }
	const std::vector<std::string> & label_names() const
	{
		return label_names_;
	}

private:
	const mir_model::MirProgram & program_;
	bool allow_external_;
	std::map<std::string, int> symbol_labels_;
	std::set<std::string> function_names_;
	std::map<std::string, int> pool_labels_;
	std::vector<PoolEntry> pool_;
	std::vector<std::string> label_names_;  // label id -> name ("" pool)
	int next_label_;
};

}  // namespace mir_native
