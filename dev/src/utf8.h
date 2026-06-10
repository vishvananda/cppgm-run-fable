#pragma once

#include <string>
#include <vector>

// Decodes a UTF-8 byte sequence into Unicode code points. Sequence
// structure is enforced (truncated sequences, stray continuation bytes in
// 0xA0-0xBF, and lead bytes above 0xF7 throw std::runtime_error), but
// stray bytes in 0x80-0x9F fall back to their Windows-1252 meaning so
// hosted sources with stray legacy punctuation still decode. Code point
// range is not checked here; out-of-range values are rejected only when
// they are encoded into output.
std::vector<int> DecodeUtf8(const std::string& bytes);

// Appends the UTF-8 encoding of one code point to out. Throws
// std::runtime_error for negative code points and those at or above
// U+10FFFF (the reference toolchain's encodable bound).
void EncodeUtf8(int code_point, std::string& out);

// Encodes the half-open range [begin, end) of code points as UTF-8.
std::string EncodeUtf8Range(const std::vector<int>& code_points,
                            size_t begin, size_t end);
