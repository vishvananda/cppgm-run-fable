#include "text_literals.h"

#include <cstdint>
#include <stdexcept>

using std::uint32_t;

#include "lex_char_classes.h"
#include "utf8.h"

namespace {

// Course-defined valid range for character-literal code points:
// [0, 0xD800) and [0xE000, 0x110000). String literals are not checked
// against this range: the reference encodes whatever the escapes
// produced (lone surrogates included) and only the UTF-8 encoder's own
// bound can reject, as a hard error rather than an invalid token.
bool IsValidLiteralCodePoint(int cp)
{
	return (cp >= 0 && cp < 0xD800) ||
		(cp >= 0xE000 && cp < 0x110000);
}

int SimpleEscapeValue(int c)
{
	switch (c)
	{
	case '\'': return 0x27;
	case '"': return 0x22;
	case '?': return 0x3F;
	case '\\': return 0x5C;
	case 'a': return 0x07;
	case 'b': return 0x08;
	case 'f': return 0x0C;
	case 'n': return 0x0A;
	case 'r': return 0x0D;
	case 't': return 0x09;
	case 'v': return 0x0B;
	default: throw std::logic_error("not a simple escape");
	}
}

// Decodes the escape-sequence whose backslash is at pos and advances pos
// past it. False for a malformed escape or a value beyond the Unicode
// range. `numeric` reports octal/hex escapes, whose values are code
// units rather than code points (2.14.3p4).
bool DecodeEscapeSequence(const string& body, size_t& pos, int& cp,
                          bool& numeric)
{
	numeric = false;
	pos++;
	if (pos >= body.size())
		return false;
	int c = static_cast<unsigned char>(body[pos]);
	if (IsSimpleEscapeChar(c))
	{
		cp = SimpleEscapeValue(c);
		pos++;
		return true;
	}
	if (IsOctalDigit(c))
	{
		numeric = true;
		cp = 0;
		for (int digits = 0;
		     digits < 3 && pos < body.size() &&
		     IsOctalDigit(static_cast<unsigned char>(body[pos]));
		     digits++, pos++)
			cp = cp * 8 + (body[pos] - '0');
		return true;
	}
	if (c == 'x')
	{
		numeric = true;
		pos++;
		if (pos >= body.size() ||
		    !IsHexDigit(static_cast<unsigned char>(body[pos])))
			return false;
		// the reference accumulates into a wrapping 32-bit value
		// ('\x100000000' is NUL); range errors surface later
		uint32_t value = 0;
		for (; pos < body.size() &&
		       IsHexDigit(static_cast<unsigned char>(body[pos]));
		     pos++)
			value = value * 16 + HexDigitValue(body[pos]);
		cp = static_cast<int>(value);
		return true;
	}
	return false;
}

// Decodes a literal body into code points. Raw bodies are plain UTF-8;
// non-raw bodies interleave escape-sequences (universal-character-names
// were already decoded in phase 1). False if any element is malformed.
// `numeric`, when given, records which elements came from octal/hex
// escapes (code-unit values, not code points).
bool DecodeBody(const string& body, bool raw, std::vector<int>& cps,
                std::vector<bool>* numeric = 0)
{
	size_t pos = 0;
	while (pos < body.size())
	{
		int cp;
		bool is_numeric = false;
		if (!raw && body[pos] == '\\')
		{
			if (!DecodeEscapeSequence(body, pos, cp, is_numeric))
				return false;
		}
		else if (!TryDecodeUtf8Char(body, pos, cp))
			return false;
		cps.push_back(cp);
		if (numeric)
			numeric->push_back(is_numeric);
	}
	return true;
}

// The reference extracts a ud-suffix by scanning ASCII identifier
// characters forward from the closing quote. PA1 may have attached
// further non-ASCII identifier characters to the preprocessing-token;
// those stay in the token source but are dropped from the analysis
// entirely ("abc"é is a plain string literal, "abc"_é has ud-suffix _).
string ExtractAsciiUdSuffix(const string& source, size_t close)
{
	size_t end = close + 1;
	while (end < source.size() &&
	       (IsNondigit(source[end]) || IsDigit(source[end])))
		end++;
	return source.substr(close + 1, end - (close + 1));
}

struct CharLiteralParts
{
	string prefix;  // "", "u", "U", "L"
	string body;
	string ud_suffix;
};

bool ParseCharLiteral(const string& source, CharLiteralParts& parts)
{
	size_t open = source.find('\'');
	size_t close = source.rfind('\'');
	if (open == string::npos || close == open)
		return false;
	parts.prefix = source.substr(0, open);
	parts.body = source.substr(open + 1, close - open - 1);
	parts.ud_suffix = ExtractAsciiUdSuffix(source, close);
	return parts.prefix.empty() || parts.prefix == "u" ||
		parts.prefix == "U" || parts.prefix == "L";
}

struct StringLiteralParts
{
	string prefix;  // "", "u8", "u", "U", "L"
	bool raw;
	string body;
	string ud_suffix;
};

bool IsEncodingPrefix(const string& prefix)
{
	return prefix.empty() || prefix == "u8" || prefix == "u" ||
		prefix == "U" || prefix == "L";
}

bool ParseRawStringBody(const string& source, size_t open, size_t close,
                        StringLiteralParts& parts)
{
	size_t paren = source.find('(', open + 1);
	if (paren == string::npos || paren >= close)
		return false;
	string delim = source.substr(open + 1, paren - open - 1);
	if (close < paren + 2 + delim.size())
		return false;
	size_t close_paren = close - 1 - delim.size();
	if (source[close_paren] != ')' ||
	    source.compare(close_paren + 1, delim.size(), delim) != 0)
		return false;
	parts.body = source.substr(paren + 1, close_paren - paren - 1);
	return true;
}

bool ParseStringLiteral(const string& source, StringLiteralParts& parts)
{
	size_t open = source.find('"');
	size_t close = source.rfind('"');
	if (open == string::npos || close == open)
		return false;
	string head = source.substr(0, open);
	parts.raw = !head.empty() && head[head.size() - 1] == 'R';
	parts.prefix = parts.raw ? head.substr(0, head.size() - 1) : head;
	parts.ud_suffix = ExtractAsciiUdSuffix(source, close);
	if (!IsEncodingPrefix(parts.prefix))
		return false;
	if (!parts.raw)
	{
		parts.body = source.substr(open + 1, close - open - 1);
		return true;
	}
	return ParseRawStringBody(source, open, close, parts);
}

// UTF-16 code units exactly as the reference forms them: the code point
// is treated as unsigned, lone surrogates stay single units, and
// out-of-range values go through the surrogate-pair bit operations with
// natural 16-bit truncation rather than being rejected.
void AppendUtf16(int cp, string& data)
{
	uint32_t value = static_cast<uint32_t>(cp);
	if (value < 0x10000)
	{
		data += LittleEndianBytes(value, 2);
		return;
	}
	uint32_t v = value - 0x10000;
	data += LittleEndianBytes((0xD800 | (v >> 10)) & 0xFFFF, 2);
	data += LittleEndianBytes(0xDC00 | (v & 0x3FF), 2);
}

EFundamentalType StringElementType(const string& prefix, size_t& unit_size)
{
	if (prefix == "u")
	{
		unit_size = 2;
		return FT_CHAR16_T;
	}
	if (prefix == "U")
	{
		unit_size = 4;
		return FT_CHAR32_T;
	}
	if (prefix == "L")
	{
		unit_size = 4;
		return FT_WCHAR_T;
	}
	unit_size = 1;  // ordinary and u8: execution charset is UTF-8
	return FT_CHAR;
}

// Throws (via EncodeUtf8) for code points the narrow execution charset
// cannot hold; like the reference, that is a translation error rather
// than an invalid token. UTF-16/32 truncate instead. Numeric escapes
// whose value fits one narrow code unit are stored directly (2.14.5p15:
// the escape value is the code unit, not a code point to re-encode).
string EncodeStringData(const std::vector<int>& cps,
                        const std::vector<bool>& numeric, size_t unit_size)
{
	string data;
	for (size_t i = 0; i < cps.size(); i++)
	{
		int cp = cps[i];
		if (unit_size == 1)
		{
			if (i < numeric.size() && numeric[i] && cp >= 0 &&
			    cp <= 0xFF)
				data += static_cast<char>(cp);
			else
				EncodeUtf8(cp, data);
		}
		else if (unit_size == 2)
			AppendUtf16(cp, data);
		else
			data += LittleEndianBytes(static_cast<uint32_t>(cp), 4);
	}
	return data;
}

string JoinSources(const std::vector<string>& sources)
{
	string joined;
	for (size_t i = 0; i < sources.size(); i++)
	{
		if (i > 0)
			joined += ' ';
		joined += sources[i];
	}
	return joined;
}

// Folds one member literal into the sequence-wide encoding-prefix and
// ud-suffix per the course concatenation rules: at most one distinct
// non-empty value of each across the sequence.
bool CombineSequenceAttribute(const string& value, string& combined)
{
	if (value.empty())
		return true;
	if (combined.empty())
		combined = value;
	return combined == value;
}

} // namespace

CharLiteralAnalysis AnalyzeCharLiteral(const string& source)
{
	CharLiteralAnalysis result;
	result.body_invalid = false;
	CharLiteralParts parts;
	if (!ParseCharLiteral(source, parts))
	{
		result.token = MakeInvalidToken(source);
		return result;
	}
	// The body is checked before the ud-suffix ('ab'q fails as a body,
	// not a suffix, so it does not interrupt a string sequence).
	std::vector<int> cps;
	if (!DecodeBody(parts.body, false, cps) || cps.size() != 1 ||
	    !IsValidLiteralCodePoint(cps[0]))
	{
		result.token = MakeInvalidToken(source);
		result.body_invalid = true;
		return result;
	}
	int cp = cps[0];
	if (!parts.ud_suffix.empty() && parts.ud_suffix[0] != '_')
	{
		result.token = MakeInvalidToken(source);
		return result;
	}

	PostToken token;
	token.kind = parts.ud_suffix.empty() ? PTK_LITERAL : PTK_UD_CHARACTER;
	token.source = source;
	token.ud_suffix = parts.ud_suffix;
	size_t size;
	if (parts.prefix.empty())
	{
		// course rule: ASCII stays char, other code points are int
		token.type = cp <= 127 ? FT_CHAR : FT_INT;
		size = cp <= 127 ? 1 : 4;
	}
	else if (parts.prefix == "u")
	{
		if (cp >= 0x10000)  // must fit one UTF-16 code unit
		{
			result.token = MakeInvalidToken(source);
			return result;
		}
		token.type = FT_CHAR16_T;
		size = 2;
	}
	else if (parts.prefix == "U")
	{
		token.type = FT_CHAR32_T;
		size = 4;
	}
	else
	{
		token.type = FT_WCHAR_T;
		size = 4;
	}
	token.data = LittleEndianBytes(cp, size);
	result.token = token;
	return result;
}

PostToken AnalyzeStringSequence(const std::vector<string>& sources)
{
	string joined = JoinSources(sources);
	string prefix;
	string ud_suffix;
	std::vector<int> cps;
	std::vector<bool> numeric;
	for (const string& source : sources)
	{
		StringLiteralParts parts;
		if (!ParseStringLiteral(source, parts))
			return MakeInvalidToken(joined);
		if (!CombineSequenceAttribute(parts.prefix, prefix) ||
		    !CombineSequenceAttribute(parts.ud_suffix, ud_suffix))
			return MakeInvalidToken(joined);
		if (!DecodeBody(parts.body, parts.raw, cps, &numeric))
			return MakeInvalidToken(joined);
	}
	cps.push_back(0);
	numeric.push_back(false);

	// Reference check order: encode first (a UTF-8 range failure is a
	// hard translation error even when the suffix below would have made
	// the sequence invalid), then require the combined ud-suffix to
	// start with an underscore.
	size_t unit_size;
	EFundamentalType type = StringElementType(prefix, unit_size);
	string data = EncodeStringData(cps, numeric, unit_size);
	if (!ud_suffix.empty() && ud_suffix[0] != '_')
		return MakeInvalidToken(joined);

	PostToken token;
	token.kind = ud_suffix.empty() ? PTK_LITERAL_ARRAY : PTK_UD_STRING;
	token.source = joined;
	token.ud_suffix = ud_suffix;
	token.type = type;
	token.data = data;
	token.num_elements = token.data.size() / unit_size;
	return token;
}
