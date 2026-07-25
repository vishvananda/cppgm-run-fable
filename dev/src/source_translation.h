#pragma once

#include <cstdint>
#include <string>
#include <vector>

// In-band markers stored in TranslatedChar::cp alongside real code points.
//
// kEndOfInputChar is produced by the universal-character-name whose eight
// hex digits are all F (the value wraps to -1); the reference pipeline
// treats it as end of input, so tokenization stops there and the rest of
// the file is ignored. (The marker is deliberately not spelled literally
// here: phases 1-2 decode UCNs before comments are stripped, so a raw
// spelling would truncate this very file.)
//
// kInvalidChar marks a malformed source byte. Translation cannot reject it
// outright: a bad byte is an error only if tokenization actually reads it
// in translated mode. Inside a raw string literal the same bytes are
// rescanned in raw mode instead, where ordinary UTF-8 decoding applies.
const int kEndOfInputChar = -1;
const int kInvalidChar = -2;

// One translated code point together with the half-open range of source
// bytes (TranslatedSource::bytes offsets) it was produced from. The mapping
// lets the tokenizer revert translations inside raw string literals by
// rescanning the byte stream, then resuming the translated stream after
// the closing quote.
//
// The offsets are 32-bit on purpose: chars is the dominant allocation of
// every tool run (one entry per code point of the whole file), and
// size_t offsets made the 12MB stress inputs peak at ~0.4GB. TranslateSource
// rejects inputs that could overflow them.
struct TranslatedChar
{
	int cp;
	uint32_t src_begin;
	uint32_t src_end;
};

struct TranslatedSource
{
	// Phase-1 byte stream: the source file image with a missing final
	// line feed appended. Raw string literals are tokenized directly
	// over this. A leading BOM is still present in the bytes; the
	// translator drops it from chars.
	std::string bytes;
	// Phase 1-2 result: UTF-8 decoded interleaved with trigraph
	// replacement, line splicing, and universal-character-name decoding.
	// The interleaving is observable: characters consumed as lookahead
	// (after a backslash, a lone '?', or a splice) are read as single
	// raw bytes, identity-mapped to code points, not UTF-8 decoded.
	std::vector<TranslatedChar> chars;
};

// Applies translation phases 1 and 2 to a UTF-8 source file image.
// Malformed bytes never throw; they become kInvalidChar entries (see
// above). The only throw is a length_error for inputs too large for the
// 32-bit provenance offsets (>= 4GB).
TranslatedSource TranslateSource(const std::string& input);
