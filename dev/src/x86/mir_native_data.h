#pragma once

#include <cstddef>
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

}  // namespace mir_native
