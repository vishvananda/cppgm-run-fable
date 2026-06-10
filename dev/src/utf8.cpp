#include "utf8.h"

#include <stdexcept>

namespace {

// Windows-1252 mapping for the C1 range 0x80-0x9F; -1 marks bytes with no
// Windows-1252 character. Stray bytes in this range fall back to this table
// so hosted sources with stray Windows-1252 punctuation still decode; the
// rest of the byte space keeps strict UTF-8 sequence structure.
const int kWindows1252C1[32] =
{
	0x20AC,     -1, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,     -1, 0x017D,     -1,
	    -1, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153,     -1, 0x017E, 0x0178
};

int ContinuationValue(const std::string& bytes, size_t pos)
{
	if (pos >= bytes.size())
		throw std::runtime_error("invalid UTF-8: truncated sequence");
	unsigned char unit = bytes[pos];
	if ((unit & 0xC0) != 0x80)
		throw std::runtime_error("invalid UTF-8: missing continuation byte");
	return unit & 0x3F;
}

// Decodes one code point starting at pos and advances pos past it.
int DecodeOne(const std::string& bytes, size_t& pos)
{
	unsigned char lead = bytes[pos];
	if (lead < 0x80)
	{
		pos += 1;
		return lead;
	}
	if (lead < 0xA0)
	{
		int code_point = kWindows1252C1[lead - 0x80];
		if (code_point < 0)
			throw std::runtime_error("invalid UTF-8: stray byte");
		pos += 1;
		return code_point;
	}
	if (lead < 0xC0)
		throw std::runtime_error("invalid UTF-8: stray continuation byte");
	if (lead < 0xE0)
	{
		int code_point = (lead & 0x1F) << 6;
		code_point |= ContinuationValue(bytes, pos + 1);
		pos += 2;
		return code_point;
	}
	if (lead < 0xF0)
	{
		int code_point = (lead & 0x0F) << 12;
		code_point |= ContinuationValue(bytes, pos + 1) << 6;
		code_point |= ContinuationValue(bytes, pos + 2);
		pos += 3;
		return code_point;
	}
	if (lead < 0xF8)
	{
		int code_point = (lead & 0x07) << 18;
		code_point |= ContinuationValue(bytes, pos + 1) << 12;
		code_point |= ContinuationValue(bytes, pos + 2) << 6;
		code_point |= ContinuationValue(bytes, pos + 3);
		pos += 4;
		return code_point;
	}
	throw std::runtime_error("invalid UTF-8: bad lead byte");
}

} // namespace

std::vector<int> DecodeUtf8(const std::string& bytes)
{
	std::vector<int> code_points;
	code_points.reserve(bytes.size());
	size_t pos = 0;
	while (pos < bytes.size())
		code_points.push_back(DecodeOne(bytes, pos));
	return code_points;
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

std::string EncodeUtf8Range(const std::vector<int>& code_points,
                            size_t begin, size_t end)
{
	std::string out;
	for (size_t i = begin; i < end && i < code_points.size(); i++)
		EncodeUtf8(code_points[i], out);
	return out;
}
