#include "lex_char_classes.h"

#include <cstddef>
#include <utility>

namespace {

// See C++ standard 2.11 Identifiers and Annex E.1
const std::pair<int, int> kAnnexE1AllowedRanges[] =
{
	{0xA8,0xA8}, {0xAA,0xAA}, {0xAD,0xAD}, {0xAF,0xAF}, {0xB2,0xB5},
	{0xB7,0xBA}, {0xBC,0xBE}, {0xC0,0xD6}, {0xD8,0xF6}, {0xF8,0xFF},
	{0x100,0x167F}, {0x1681,0x180D}, {0x180F,0x1FFF}, {0x200B,0x200D},
	{0x202A,0x202E}, {0x203F,0x2040}, {0x2054,0x2054}, {0x2060,0x206F},
	{0x2070,0x218F}, {0x2460,0x24FF}, {0x2776,0x2793}, {0x2C00,0x2DFF},
	{0x2E80,0x2FFF}, {0x3004,0x3007}, {0x3021,0x302F}, {0x3031,0x303F},
	{0x3040,0xD7FF}, {0xF900,0xFD3D}, {0xFD40,0xFDCF}, {0xFDF0,0xFE44},
	{0xFE47,0xFFFD},
	{0x10000,0x1FFFD}, {0x20000,0x2FFFD}, {0x30000,0x3FFFD},
	{0x40000,0x4FFFD}, {0x50000,0x5FFFD}, {0x60000,0x6FFFD},
	{0x70000,0x7FFFD}, {0x80000,0x8FFFD}, {0x90000,0x9FFFD},
	{0xA0000,0xAFFFD}, {0xB0000,0xBFFFD}, {0xC0000,0xCFFFD},
	{0xD0000,0xDFFFD}, {0xE0000,0xEFFFD}
};

// See C++ standard 2.11 Identifiers and Annex E.2
const std::pair<int, int> kAnnexE2DisallowedInitiallyRanges[] =
{
	{0x300,0x36F}, {0x1DC0,0x1DFF}, {0x20D0,0x20FF}, {0xFE20,0xFE2F}
};

template <size_t N>
bool InSortedRanges(int cp, const std::pair<int, int> (&ranges)[N])
{
	size_t lo = 0;
	size_t hi = N;
	while (lo < hi)
	{
		size_t mid = (lo + hi) / 2;
		if (ranges[mid].second < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < N && ranges[lo].first <= cp && cp <= ranges[lo].second;
}

} // namespace

bool IsDigit(int c)
{
	return c >= '0' && c <= '9';
}

bool IsOctalDigit(int c)
{
	return c >= '0' && c <= '7';
}

bool IsHexDigit(int c)
{
	return HexDigitValue(c) >= 0;
}

int HexDigitValue(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool IsNondigit(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentifierStart(int c)
{
	if (IsNondigit(c))
		return true;
	return c >= 0x80 && InSortedRanges(c, kAnnexE1AllowedRanges) &&
		!InSortedRanges(c, kAnnexE2DisallowedInitiallyRanges);
}

bool IsIdentifierContinue(int c)
{
	if (IsNondigit(c) || IsDigit(c))
		return true;
	return c >= 0x80 && InSortedRanges(c, kAnnexE1AllowedRanges);
}

bool IsSimpleEscapeChar(int c)
{
	switch (c)
	{
	case '\'': case '"': case '?': case '\\':
	case 'a': case 'b': case 'f': case 'n': case 'r': case 't': case 'v':
		return true;
	default:
		return false;
	}
}
