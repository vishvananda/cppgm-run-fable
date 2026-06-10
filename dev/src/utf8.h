#pragma once

#include <string>

// Decodes one code point starting at pos and advances pos past it. Returns
// false (advancing pos by one byte) for malformed input: truncated
// sequences, missing continuation bytes, stray bytes in 0xA0-0xBF, lead
// bytes above 0xF7, and bytes in 0x80-0x9F with no Windows-1252 meaning.
// Stray bytes in 0x80-0x9F otherwise fall back to their Windows-1252
// character so hosted sources with stray legacy punctuation still decode.
// Code point range is not checked (overlong forms and surrogates decode);
// out-of-range values are rejected only when encoded into output.
bool TryDecodeUtf8Char(const std::string& bytes, size_t& pos, int& code_point);

// As TryDecodeUtf8Char, but throws std::runtime_error on malformed input.
int DecodeUtf8Char(const std::string& bytes, size_t& pos);

// Appends the UTF-8 encoding of one code point to out. Throws
// std::runtime_error for negative code points and those at or above
// U+10FFFF (the reference toolchain's encodable bound).
void EncodeUtf8(int code_point, std::string& out);

// Decodes the half-open byte range [begin, end) and re-encodes it as UTF-8
// (Windows-1252 fallback bytes change spelling). Throws std::runtime_error
// on malformed input or unencodable code points.
std::string ReencodeUtf8Range(const std::string& bytes,
                              size_t begin, size_t end);
