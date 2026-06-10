#include "source_translation.h"

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

// One combined left-to-right pass for trigraphs, line splices, and
// universal-character-names. A single pass matches the reference pipeline,
// whose backslash handler consumes its lookahead: characters examined after
// a backslash that fails to splice or form a UCN are emitted untranslated,
// so they never start a trigraph or splice of their own.
class Translator
{
public:
	Translator(const std::vector<int>& raw, std::vector<TranslatedChar>& out)
		: raw_(raw), out_(out)
	{}

	void Run()
	{
		size_t i = 0;
		while (i < raw_.size())
			i = TranslateAt(i);
	}

private:
	void Emit(int cp, size_t begin, size_t end)
	{
		out_.push_back({cp, begin, end});
	}

	size_t TranslateAt(size_t i)
	{
		if (raw_[i] == '?')
			return TranslateQuestion(i);
		if (raw_[i] == '\\')
			return TranslateBackslash(i, i + 1);
		Emit(raw_[i], i, i + 1);
		return i + 1;
	}

	// "??X" forming a trigraph translates; "??" not completing one emits
	// the first '?' and re-examines from the second; "?X" emits both
	// characters untranslated (the consumed lookahead never starts a
	// trigraph, splice, or universal-character-name of its own).
	size_t TranslateQuestion(size_t i)
	{
		if (i + 1 < raw_.size() && raw_[i + 1] == '?')
		{
			int replacement = i + 2 < raw_.size()
				? TrigraphReplacement(raw_[i + 2]) : -1;
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
		if (i + 1 >= raw_.size())
			return i + 1;
		Emit(raw_[i + 1], i + 1, i + 2);
		return i + 2;
	}

	// A backslash spelled raw_[begin, j) (one character, or three for the
	// trigraph ??/) followed by a new-line splices; followed by u or U it
	// may form a universal-character-name; any other following character
	// is consumed together with the backslash as two ordinary untranslated
	// characters.
	size_t TranslateBackslash(size_t begin, size_t j)
	{
		if (j >= raw_.size())
		{
			Emit('\\', begin, j);
			return j;
		}
		if (raw_[j] == '\n')
			return TranslateAfterSplice(j + 1);
		if (raw_[j] == 'u' || raw_[j] == 'U')
			return TranslateUcn(begin, j);
		Emit('\\', begin, j);
		Emit(raw_[j], j, j + 1);
		return j + 1;
	}

	// After a splice the next character is read raw: only a backslash
	// re-enters translation (a further splice or a UCN); anything else,
	// including a '?', is emitted untranslated.
	size_t TranslateAfterSplice(size_t i)
	{
		if (i >= raw_.size())
			return i;
		if (raw_[i] == '\\')
			return TranslateBackslash(i, i + 1);
		Emit(raw_[i], i, i + 1);
		return i + 1;
	}

	// The hex-quad scan reads raw characters: trigraphs and splices are
	// not recognized inside a universal-character-name.
	size_t TranslateUcn(size_t begin, size_t j)
	{
		size_t digits = raw_[j] == 'u' ? 4 : 8;
		unsigned value = 0;
		size_t k = 0;
		while (k < digits && j + 1 + k < raw_.size() &&
			HexDigitValue(raw_[j + 1 + k]) >= 0)
		{
			value = value * 16 + HexDigitValue(raw_[j + 1 + k]);
			k++;
		}
		if (k == digits)
		{
			// Stored as a plain int: \UFFFFFFFF becomes -1, the
			// in-band end-of-input marker, and other out-of-range
			// values are rejected only if they reach token data.
			Emit(static_cast<int>(value), begin, j + 1 + digits);
			return j + 1 + digits;
		}
		// Incomplete: the backslash, the u/U, the scanned hex prefix,
		// and the character that stopped the scan are all consumed
		// untranslated.
		Emit('\\', begin, j);
		size_t end = j + 1 + k;
		if (end < raw_.size())
			end++;
		for (size_t p = j; p < end; p++)
			Emit(raw_[p], p, p + 1);
		return end;
	}

	const std::vector<int>& raw_;
	std::vector<TranslatedChar>& out_;
};

} // namespace

TranslatedSource TranslateSource(const std::string& input)
{
	TranslatedSource source;
	source.raw = DecodeUtf8(input);
	bool any_input = !source.raw.empty();
	if (any_input && source.raw[0] == 0xFEFF)
		source.raw.erase(source.raw.begin());
	// A non-empty file (even one reduced to nothing by the BOM strip) gets
	// its missing final line feed before splicing; a splice that consumes
	// it is not restored.
	if (any_input && (source.raw.empty() || source.raw.back() != '\n'))
		source.raw.push_back('\n');
	Translator(source.raw, source.chars).Run();
	return source;
}
