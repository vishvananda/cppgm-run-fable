#include "pp_tokenizer.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utf8.h"

namespace {

const int kEndOfInput = -1;

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
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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

// Whitespace other than new-line. Deliberately not isspace(): membership
// must not depend on locale and must not include code points above 0x7F.
bool IsBasicWhitespace(int c)
{
	return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

// See C++ standard 2.13 Operators and punctuators
const std::unordered_set<string>& IdentifierLikeOperators()
{
	static const std::unordered_set<string> ops =
	{
		"new", "delete", "and", "and_eq", "bitand", "bitor", "compl",
		"not", "not_eq", "or", "or_eq", "xor", "xor_eq"
	};
	return ops;
}

const std::unordered_set<string>& SymbolOperators()
{
	static const std::unordered_set<string> ops =
	{
		"{", "}", "[", "]", "#", "##", "(", ")", "<:", ":>", "<%", "%>",
		"%:", "%:%:", ";", ":", "...", "?", "::", ".", ".*",
		"+", "-", "*", "/", "%", "^", "&", "|", "~", "!", "=", "<", ">",
		"+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", "<<", ">>",
		">>=", "<<=", "<=", ">=", "&&", "==", "!=", "||", "++", "--",
		",", "->*", "->"
	};
	return ops;
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

bool IsOpStartChar(int c)
{
	switch (c)
	{
	case '{': case '}': case '[': case ']': case '#': case '(': case ')':
	case ';': case ':': case '?': case '.': case '+': case '-': case '*':
	case '/': case '%': case '^': case '&': case '|': case '~': case '!':
	case '=': case '<': case '>': case ',':
		return true;
	default:
		return false;
	}
}

class PPTokenizer
{
public:
	PPTokenizer(const TranslatedSource& source, IPPTokenStream& output)
		: source_(source), output_(output), pos_(0),
		  include_state_(kLineStart)
	{}

	void Run()
	{
		while (pos_ < Chars().size() && Peek(0) != kEndOfInput)
			ScanToken();
		output_.emit_eof();
	}

private:
	// Header-name context: a header-name is only recognized after
	// (start of file or new-line) (# or %:) include, disregarding
	// whitespace-sequences (16.2).
	enum IncludeState { kLineStart, kAfterHash, kAfterInclude, kNormal };

	const std::vector<TranslatedChar>& Chars() const
	{
		return source_.chars;
	}

	int Peek(size_t ahead) const
	{
		size_t i = pos_ + ahead;
		return i < Chars().size() ? Chars()[i].cp : kEndOfInput;
	}

	string EncodeRange(size_t begin, size_t end) const
	{
		string out;
		for (size_t i = begin; i < end && i < Chars().size(); i++)
			EncodeUtf8(Chars()[i].cp, out);
		return out;
	}

	void ScanToken()
	{
		int c = Peek(0);
		if (c == '\n')
		{
			pos_++;
			output_.emit_new_line();
			include_state_ = kLineStart;
			return;
		}
		if (IsBasicWhitespace(c) ||
			(c == '/' && (Peek(1) == '/' || Peek(1) == '*')))
		{
			ScanWhitespaceSequence();
			return;
		}
		if (IsIdentifierStart(c))
		{
			ScanIdentifierOrPrefixedLiteral();
			return;
		}
		if (IsDigit(c) || (c == '.' && IsDigit(Peek(1))))
		{
			ScanPPNumber();
			return;
		}
		if (c == '\'')
		{
			ScanCharLiteral(pos_);
			return;
		}
		if (c == '"')
		{
			if (include_state_ == kAfterInclude)
				ScanHeaderName('"');
			else
				ScanStringLiteral(pos_);
			return;
		}
		if (c == '<' && include_state_ == kAfterInclude)
		{
			ScanHeaderName('>');
			return;
		}
		if (IsOpStartChar(c))
		{
			ScanOpOrPunc();
			return;
		}
		string data;
		EncodeUtf8(c, data);
		pos_++;
		output_.emit_non_whitespace_char(data);
		include_state_ = kNormal;
	}

	// Comments are subsumed into the whitespace-sequence; a block comment
	// swallows interior new-lines.
	void ScanWhitespaceSequence()
	{
		while (true)
		{
			int c = Peek(0);
			if (IsBasicWhitespace(c))
			{
				pos_++;
				continue;
			}
			if (c == '/' && Peek(1) == '*')
			{
				SkipBlockComment();
				continue;
			}
			if (c == '/' && Peek(1) == '/')
			{
				SkipLineComment();
				continue;
			}
			break;
		}
		output_.emit_whitespace_sequence();
	}

	void SkipBlockComment()
	{
		size_t i = pos_ + 2;
		while (i < Chars().size() && Chars()[i].cp != kEndOfInput)
		{
			if (Chars()[i].cp == '*' && i + 1 < Chars().size() &&
				Chars()[i + 1].cp == '/')
			{
				pos_ = i + 2;
				return;
			}
			i++;
		}
		throw std::runtime_error("unterminated comment");
	}

	// A line comment must be terminated by a new-line; reaching the end of
	// input (a trailing splice can consume the final line feed) is an
	// error, matching the reference.
	void SkipLineComment()
	{
		size_t i = pos_ + 2;
		while (i < Chars().size() && Chars()[i].cp != kEndOfInput)
		{
			if (Chars()[i].cp == '\n')
			{
				pos_ = i;
				return;
			}
			i++;
		}
		throw std::runtime_error("unterminated comment");
	}

	void ScanIdentifierOrPrefixedLiteral()
	{
		size_t begin = pos_;
		pos_++;
		while (IsIdentifierContinue(Peek(0)))
			pos_++;
		string ident = EncodeRange(begin, pos_);
		int next = Peek(0);
		if (next == '"')
		{
			if (ident == "R" || ident == "uR" || ident == "u8R" ||
				ident == "UR" || ident == "LR")
			{
				ScanRawStringLiteral(begin);
				return;
			}
			if (ident == "u8" || ident == "u" || ident == "U" ||
				ident == "L")
			{
				ScanStringLiteral(begin);
				return;
			}
		}
		if (next == '\'' &&
			(ident == "u" || ident == "U" || ident == "L"))
		{
			ScanCharLiteral(begin);
			return;
		}
		if (IdentifierLikeOperators().count(ident))
		{
			output_.emit_preprocessing_op_or_punc(ident);
			include_state_ = kNormal;
			return;
		}
		output_.emit_identifier(ident);
		include_state_ =
			(include_state_ == kAfterHash && ident == "include")
				? kAfterInclude : kNormal;
	}

	// A pp-number whose first x/X precedes any dot lexes as a hexadecimal
	// literal: its exponent character is p/P instead of e/E (matches the
	// reference tokenizer; plain pp-numbers take a sign only after e/E).
	void ScanPPNumber()
	{
		size_t begin = pos_;
		bool dot_seen = false;
		bool hex_literal = false;
		if (Peek(0) == '.')
		{
			dot_seen = true;
			pos_++;
		}
		pos_++;
		while (true)
		{
			int c = Peek(0);
			bool exponent = hex_literal ? (c == 'p' || c == 'P')
			                            : (c == 'e' || c == 'E');
			if (exponent)
			{
				pos_++;
				if (Peek(0) == '+' || Peek(0) == '-')
					pos_++;
				continue;
			}
			if (c == '.')
			{
				dot_seen = true;
				pos_++;
				continue;
			}
			if (IsIdentifierContinue(c))
			{
				if ((c == 'x' || c == 'X') && !dot_seen)
					hex_literal = true;
				pos_++;
				continue;
			}
			break;
		}
		output_.emit_pp_number(EncodeRange(begin, pos_));
		include_state_ = kNormal;
	}

	// Validates one escape-sequence starting at the current backslash and
	// advances past it. Valid universal-character-names were already
	// decoded during translation, so a remaining \u or \U is ill-formed.
	void ScanEscapeSequence()
	{
		int c = Peek(1);
		if (IsSimpleEscapeChar(c))
		{
			pos_ += 2;
			return;
		}
		if (IsOctalDigit(c))
		{
			pos_ += 2;
			for (int k = 0; k < 2 && IsOctalDigit(Peek(0)); k++)
				pos_++;
			return;
		}
		if (c == 'x')
		{
			pos_ += 2;
			if (!IsHexDigit(Peek(0)))
				throw std::runtime_error("invalid hex escape sequence");
			while (IsHexDigit(Peek(0)))
				pos_++;
			return;
		}
		throw std::runtime_error("invalid escape sequence");
	}

	// Consumes a ud-suffix if the next character starts an identifier.
	bool ScanUdSuffix()
	{
		if (!IsIdentifierStart(Peek(0)))
			return false;
		pos_++;
		while (IsIdentifierContinue(Peek(0)))
			pos_++;
		return true;
	}

	void ScanCharLiteral(size_t begin)
	{
		pos_++;
		while (true)
		{
			int c = Peek(0);
			if (c == '\'')
			{
				pos_++;
				break;
			}
			if (c == '\n' || c == kEndOfInput)
				throw std::runtime_error(
					"unterminated character literal");
			if (c == '\\')
				ScanEscapeSequence();
			else
				pos_++;
		}
		bool user_defined = ScanUdSuffix();
		string data = EncodeRange(begin, pos_);
		if (user_defined)
			output_.emit_user_defined_character_literal(data);
		else
			output_.emit_character_literal(data);
		include_state_ = kNormal;
	}

	void ScanStringLiteral(size_t begin)
	{
		pos_++;
		while (true)
		{
			int c = Peek(0);
			if (c == '"')
			{
				pos_++;
				break;
			}
			if (c == '\n' || c == kEndOfInput)
				throw std::runtime_error("unterminated string literal");
			if (c == '\\')
				ScanEscapeSequence();
			else
				pos_++;
		}
		bool user_defined = ScanUdSuffix();
		string data = EncodeRange(begin, pos_);
		if (user_defined)
			output_.emit_user_defined_string_literal(data);
		else
			output_.emit_string_literal(data);
		include_state_ = kNormal;
	}

	// Scans the raw portion of a raw string literal over the phase-1
	// stream so trigraphs, line splices, and universal-character-names
	// are reverted (2.14.5p4), then resumes the translated stream after
	// the closing quote. begin is the translated index of the encoding
	// prefix; pos_ is at the opening quote.
	void ScanRawStringLiteral(size_t begin)
	{
		const std::vector<int>& raw = source_.raw;
		string data = EncodeRange(begin, pos_ + 1);
		size_t r = Chars()[pos_].src_end;
		size_t raw_begin = r;
		std::vector<int> delim;
		while (true)
		{
			if (r >= raw.size() || raw[r] == '\n')
				throw std::runtime_error(
					"unterminated raw string literal");
			if (raw[r] == '(')
				break;
			delim.push_back(raw[r]);
			if (delim.size() > 16)
				throw std::runtime_error(
					"raw string delimiter too long");
			r++;
		}
		size_t close_quote = FindRawClosingQuote(r + 1, delim);
		data += EncodeUtf8Range(raw, raw_begin, close_quote + 1);
		pos_ = ResumeIndexAfterRaw(close_quote + 1);
		size_t suffix_begin = pos_;
		bool user_defined = ScanUdSuffix();
		data += EncodeRange(suffix_begin, pos_);
		if (user_defined)
			output_.emit_user_defined_string_literal(data);
		else
			output_.emit_string_literal(data);
		include_state_ = kNormal;
	}

	// Returns the raw index of the closing quote of ")delim"" at or after
	// search_from.
	size_t FindRawClosingQuote(size_t search_from,
	                           const std::vector<int>& delim) const
	{
		const std::vector<int>& raw = source_.raw;
		for (size_t r = search_from; r < raw.size(); r++)
		{
			if (raw[r] != ')')
				continue;
			size_t quote = r + 1 + delim.size();
			if (quote >= raw.size() || raw[quote] != '"')
				continue;
			if (std::equal(delim.begin(), delim.end(),
			               raw.begin() + r + 1))
				return quote;
		}
		throw std::runtime_error("unterminated raw string literal");
	}

	// First translated index at or after the given raw index. src_begin
	// is nondecreasing across the translated stream.
	size_t ResumeIndexAfterRaw(size_t raw_index) const
	{
		size_t lo = 0;
		size_t hi = Chars().size();
		while (lo < hi)
		{
			size_t mid = (lo + hi) / 2;
			if (Chars()[mid].src_begin < raw_index)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo;
	}

	void ScanHeaderName(int close)
	{
		size_t begin = pos_;
		size_t i = pos_ + 1;
		while (i < Chars().size() && Chars()[i].cp != '\n' &&
			Chars()[i].cp != kEndOfInput && Chars()[i].cp != close)
			i++;
		if (i >= Chars().size() || Chars()[i].cp != close)
			throw std::runtime_error("unterminated header name");
		pos_ = i + 1;
		output_.emit_header_name(EncodeRange(begin, pos_));
		include_state_ = kNormal;
	}

	void ScanOpOrPunc()
	{
		// 2.5p3: if the next three characters are <:: and the subsequent
		// character is neither : nor >, the < is a token by itself.
		if (Peek(0) == '<' && Peek(1) == ':' && Peek(2) == ':' &&
			Peek(3) != ':' && Peek(3) != '>')
		{
			pos_++;
			output_.emit_preprocessing_op_or_punc("<");
			include_state_ = kNormal;
			return;
		}
		string text;
		for (size_t k = 0; k < 4; k++)
		{
			int c = Peek(k);
			if (c <= 0x20 || c >= 0x7F)
				break;
			text += static_cast<char>(c);
		}
		for (size_t len = text.size(); len > 0; len--)
		{
			string candidate = text.substr(0, len);
			if (!SymbolOperators().count(candidate))
				continue;
			pos_ += len;
			output_.emit_preprocessing_op_or_punc(candidate);
			if ((candidate == "#" || candidate == "%:") &&
				include_state_ == kLineStart)
				include_state_ = kAfterHash;
			else
				include_state_ = kNormal;
			return;
		}
		throw std::logic_error("ScanOpOrPunc called on a non-operator");
	}

	const TranslatedSource& source_;
	IPPTokenStream& output_;
	size_t pos_;
	IncludeState include_state_;
};

} // namespace

void TokenizePPTokens(const TranslatedSource& source, IPPTokenStream& output)
{
	PPTokenizer tokenizer(source, output);
	tokenizer.Run();
}
