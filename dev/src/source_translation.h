#pragma once

#include <string>
#include <vector>

// One translated code point together with the half-open range of phase-1
// code points (TranslatedSource::raw indices) it was produced from. The
// mapping lets the tokenizer revert translations inside raw string literals
// by rescanning the raw stream, then resuming the translated stream after
// the closing quote.
struct TranslatedChar
{
	int cp;
	size_t src_begin;
	size_t src_end;
};

struct TranslatedSource
{
	// Phase-1 stream: UTF-8 decoded, leading BOM dropped, missing final
	// line feed appended.
	std::vector<int> raw;
	// Phase 1-2 result: trigraphs replaced, line splices removed,
	// universal-character-names decoded. A cp of -1 (from \UFFFFFFFF) is
	// the in-band end-of-input marker; tokenization stops there.
	std::vector<TranslatedChar> chars;
};

// Applies translation phases 1 and 2 to a UTF-8 source file image.
// Throws std::runtime_error for invalid UTF-8 input.
TranslatedSource TranslateSource(const std::string& input);
