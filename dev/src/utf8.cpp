#include "utf8.h"

#include <stdexcept>

namespace {

// Windows-1252 mapping for the C1 range 0x80-0x9F; -1 marks bytes with no
// Windows-1252 character.
const int kWindows1252C1[32] =
{
	0x20AC,     -1, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,     -1, 0x017D,     -1,
	    -1, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153,     -1, 0x017E, 0x0178
};

// Accumulates count continuation bytes into code_point; returns false if
// any byte is missing or is not a continuation byte.
bool AccumulateContinuations(const std::string& bytes, size_t pos,
                             size_t count, int& code_point)
{
	for (size_t i = 0; i < count; i++)
	{
		if (pos + i >= bytes.size())
			return false;
		unsigned char unit = bytes[pos + i];
		if ((unit & 0xC0) != 0x80)
			return false;
		code_point = (code_point << 6) | (unit & 0x3F);
	}
	return true;
}

} // namespace

bool TryDecodeUtf8Char(const std::string& bytes, size_t& pos, int& code_point)
{
	unsigned char lead = bytes[pos];
	if (lead < 0x80)
	{
		pos += 1;
		code_point = lead;
		return true;
	}
	if (lead < 0xA0)
	{
		// A C1 byte in lead position is a stray legacy byte, not UTF-8.
		pos += 1;
		code_point = kWindows1252C1[lead - 0x80];
		return code_point >= 0;
	}
	size_t continuations;
	if (lead < 0xC0)
	{
		pos += 1;
		return false;
	}
	else if (lead < 0xE0)
	{
		code_point = lead & 0x1F;
		continuations = 1;
	}
	else if (lead < 0xF0)
	{
		code_point = lead & 0x0F;
		continuations = 2;
	}
	else if (lead < 0xF8)
	{
		code_point = lead & 0x07;
		continuations = 3;
	}
	else
	{
		pos += 1;
		return false;
	}
	if (!AccumulateContinuations(bytes, pos + 1, continuations, code_point))
	{
		pos += 1;
		return false;
	}
	pos += 1 + continuations;
	return true;
}

int DecodeUtf8Char(const std::string& bytes, size_t& pos)
{
	int code_point;
	if (!TryDecodeUtf8Char(bytes, pos, code_point))
		throw std::runtime_error("invalid UTF-8 byte sequence");
	return code_point;
}

void EncodeUtf8(int code_point, std::string& out)
{
	if (code_point < 0 || code_point >= 0x10FFFF)
		throw std::runtime_error("code point not encodable as UTF-8");
	if (code_point < 0x80)
	{
		out += static_cast<char>(code_point);
		return;
	}
	if (code_point < 0x800)
	{
		out += static_cast<char>(0xC0 | (code_point >> 6));
		out += static_cast<char>(0x80 | (code_point & 0x3F));
		return;
	}
	if (code_point < 0x10000)
	{
		out += static_cast<char>(0xE0 | (code_point >> 12));
		out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (code_point & 0x3F));
		return;
	}
	out += static_cast<char>(0xF0 | (code_point >> 18));
	out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
	out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
	out += static_cast<char>(0x80 | (code_point & 0x3F));
}

std::string ReencodeUtf8Range(const std::string& bytes,
                              size_t begin, size_t end)
{
	std::string out;
	size_t pos = begin;
	while (pos < end && pos < bytes.size())
		EncodeUtf8(DecodeUtf8Char(bytes, pos), out);
	return out;
}
