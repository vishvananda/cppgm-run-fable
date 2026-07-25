#include "source_translation.h"

#include <stdexcept>

#include "utf8.h"

namespace {

// Returns the replacement for the trigraph "??c", or -1 if "??c" is not a
// trigraph.
int TrigraphReplacement(int c)
{
	switch (c)
	{
	case '=': return '#';
	case '/': return '\\';
	case '\'': return '^';
	case '(': return '[';
	case ')': return ']';
	case '!': return '|';
	case '<': return '{';
	case '>': return '}';
	case '-': return '~';
	default: return -1;
	}
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

// One combined left-to-right pass over the source bytes for UTF-8 decoding,
// trigraphs, line splices, and universal-character-names. The phases are
// interleaved at the byte level to match the reference pipeline, whose
// backslash and '?' handlers consume their lookahead as single raw bytes:
// a consumed byte is identity-mapped to a code point (so a multi-byte UTF-8
// sequence after a backslash is split, usually making the orphaned
// continuation bytes invalid), and it never starts a trigraph or splice of
// its own.
class Translator
{
public:
	Translator(const std::string& bytes, std::vector<TranslatedChar>& out)
		: bytes_(bytes), out_(out)
	{}

	void Run()
	{
		size_t i = 0;
		if (bytes_.compare(0, 3, "\xEF\xBB\xBF") == 0)
		{
			// A leading BOM is dropped, and the character after it is
			// decoded normally but consumed without phase-2
			// interpretation: it never starts a trigraph, splice, or
			// universal-character-name (reference behavior).
			i = 3;
			if (i < bytes_.size())
				i = EmitDecodedChar(i);
		}
		while (i < bytes_.size())
			i = TranslateAt(i);
	}

private:
	unsigned char ByteAt(size_t i) const
	{
		return static_cast<unsigned char>(bytes_[i]);
	}

	void Emit(int cp, size_t begin, size_t end)
	{
		out_.push_back({cp,
		                static_cast<uint32_t>(begin),
		                static_cast<uint32_t>(end)});
	}

	// Consumed-lookahead read: one raw byte, identity-mapped.
	void EmitByte(size_t i)
	{
		Emit(ByteAt(i), i, i + 1);
	}

	size_t TranslateAt(size_t i)
	{
		if (bytes_[i] == '?')
			return TranslateQuestion(i);
		if (bytes_[i] == '\\')
			return TranslateBackslash(i, i + 1);
		return EmitDecodedChar(i);
	}

	// Decodes one UTF-8 character at i (malformed bytes become
	// kInvalidChar) and emits it without phase-2 interpretation.
	size_t EmitDecodedChar(size_t i)
	{
		size_t pos = i;
		int cp;
		if (!TryDecodeUtf8Char(bytes_, pos, cp))
			cp = kInvalidChar;
		Emit(cp, i, pos);
		return pos;
	}

	// "??X" forming a trigraph translates; "??" not completing one emits
	// the first '?' and re-examines from the second; "?X" emits '?' and
	// consumes the following byte.
	size_t TranslateQuestion(size_t i)
	{
		if (i + 1 < bytes_.size() && bytes_[i + 1] == '?')
		{
			int replacement = i + 2 < bytes_.size()
				? TrigraphReplacement(ByteAt(i + 2)) : -1;
			if (replacement < 0)
			{
				Emit('?', i, i + 1);
				return i + 1;
			}
			if (replacement == '\\')
				return TranslateBackslash(i, i + 3);
			Emit(replacement, i, i + 3);
			return i + 3;
		}
		Emit('?', i, i + 1);
		if (i + 1 >= bytes_.size())
			return i + 1;
		EmitByte(i + 1);
		return i + 2;
	}

	// A backslash spelled bytes [begin, j) (one byte, or three for the
	// trigraph ??/) followed by a new-line splices; followed by u or U it
	// may form a universal-character-name; any other following byte is
	// consumed together with the backslash.
	size_t TranslateBackslash(size_t begin, size_t j)
	{
		if (j >= bytes_.size())
		{
			Emit('\\', begin, j);
			return j;
		}
		if (bytes_[j] == '\n')
			return TranslateAfterSplice(j + 1);
		if (bytes_[j] == 'u' || bytes_[j] == 'U')
			return TranslateUcn(begin, j);
		Emit('\\', begin, j);
		EmitByte(j);
		return j + 1;
	}

	// After a splice the next byte is consumed raw: only a backslash
	// re-enters translation (a further splice or a UCN); anything else,
	// including a '?' or a UTF-8 lead byte, is identity-mapped.
	size_t TranslateAfterSplice(size_t i)
	{
		if (i >= bytes_.size())
			return i;
		if (bytes_[i] == '\\')
			return TranslateBackslash(i, i + 1);
		EmitByte(i);
		return i + 1;
	}

	// The hex-quad scan reads raw bytes: trigraphs and splices are not
	// recognized inside a universal-character-name.
	size_t TranslateUcn(size_t begin, size_t j)
	{
		size_t digits = bytes_[j] == 'u' ? 4 : 8;
		unsigned value = 0;
		size_t k = 0;
		while (k < digits && j + 1 + k < bytes_.size() &&
			HexDigitValue(ByteAt(j + 1 + k)) >= 0)
		{
			value = value * 16 + HexDigitValue(ByteAt(j + 1 + k));
			k++;
		}
		if (k == digits)
		{
			// Stored as a plain int: the all-F hex quads wrap
			// to kEndOfInputChar, and other out-of-range values
			// are rejected only if they reach token data.
			Emit(static_cast<int>(value), begin, j + 1 + digits);
			return j + 1 + digits;
		}
		// Incomplete: the backslash, the u/U, the scanned hex prefix,
		// and the byte that stopped the scan are all consumed raw.
		Emit('\\', begin, j);
		size_t end = j + 1 + k;
		if (end < bytes_.size())
			end++;
		for (size_t p = j; p < end; p++)
			EmitByte(p);
		return end;
	}

	const std::string& bytes_;
	std::vector<TranslatedChar>& out_;
};

} // namespace

TranslatedSource TranslateSource(const std::string& input)
{
	TranslatedSource source;
	if (input.size() >= 0xFFFFFFFFul)
		throw std::length_error(
			"source file too large for 32-bit provenance offsets");
	source.bytes = input;
	// A non-empty file (even a bare BOM, which the translator drops) gets
	// its missing final line feed before splicing; a splice that consumes
	// it is not restored.
	if (!source.bytes.empty() && source.bytes.back() != '\n')
		source.bytes += '\n';
	// Every translated char consumes at least one source byte, so this
	// reserve is an exact upper bound: the dominant allocation of a tool
	// run neither regrows nor overshoots.
	source.chars.reserve(source.bytes.size());
	Translator(source.bytes, source.chars).Run();
	return source;
}
