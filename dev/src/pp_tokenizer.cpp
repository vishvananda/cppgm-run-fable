#include "pp_tokenizer.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lex_char_classes.h"
#include "utf8.h"

namespace {

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
		while (Peek(0) != kEndOfInputChar)
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

	// Translated code point at index i; the end of the stream reads as
	// kEndOfInputChar. Reading a malformed byte in translated mode is an
	// error (a raw string literal rescans the source bytes instead, so
	// its kInvalidChar entries are never read).
	int At(size_t i) const
	{
		if (i >= Chars().size())
			return kEndOfInputChar;
		if (Chars()[i].cp == kInvalidChar)
			throw std::runtime_error("invalid UTF-8 byte sequence");
		return Chars()[i].cp;
	}

	int Peek(size_t ahead) const
	{
		return At(pos_ + ahead);
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
		while (At(i) != kEndOfInputChar)
		{
			if (At(i) == '*' && At(i + 1) == '/')
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
		while (At(i) != kEndOfInputChar)
		{
			if (At(i) == '\n')
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

	// Reference pp-number scanning (pinned by differential probes): an
	// exponent character (e/E, or p/P once hexadecimal) acts as a marker
	// only when a decimal digit or sign follows; a marker takes the sign
	// with it. An x/X switches the literal to hexadecimal -- changing
	// the marker to p/P -- unless a dot or a real decimal exponent was
	// already seen (12x3p+4 and 1eAx2p+4 take the sign; 1.2x3p, 1e+x2p,
	// and 1E0x2p split before it).
	void ScanPPNumber()
	{
		size_t begin = pos_;
		bool dot_seen = false;
		bool hex_literal = false;
		bool exponent_seen = false;
		if (Peek(0) == '.')
		{
			dot_seen = true;
			pos_++;
		}
		pos_++;
		while (true)
		{
			int c = Peek(0);
			bool marker = hex_literal ? (c == 'p' || c == 'P')
			                          : (c == 'e' || c == 'E');
			int next = Peek(1);
			if (marker && (IsDigit(next) || next == '+' || next == '-'))
			{
				pos_ += IsDigit(next) ? 1 : 2;
				if (!hex_literal)
					exponent_seen = true;
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
				if ((c == 'x' || c == 'X') && !dot_seen && !exponent_seen)
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
			if (c == '\n' || c == kEndOfInputChar)
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
			if (c == '\n' || c == kEndOfInputChar)
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

	// Scans the raw portion of a raw string literal over the source bytes
	// so trigraphs, line splices, and universal-character-names are
	// reverted (2.14.5p4) and ordinary UTF-8 decoding applies, then
	// resumes the translated stream after the closing quote. begin is the
	// translated index of the encoding prefix; pos_ is at the opening
	// quote.
	void ScanRawStringLiteral(size_t begin)
	{
		const std::string& bytes = source_.bytes;
		string data = EncodeRange(begin, pos_ + 1);
		size_t r = Chars()[pos_].src_end;
		size_t raw_begin = r;
		std::vector<int> delim;
		while (true)
		{
			if (r >= bytes.size() || bytes[r] == '\n')
				throw std::runtime_error(
					"unterminated raw string literal");
			if (bytes[r] == '(')
				break;
			delim.push_back(DecodeUtf8Char(bytes, r));
			if (delim.size() > 16)
				throw std::runtime_error(
					"raw string delimiter too long");
		}
		size_t close_quote = FindRawClosingQuote(r + 1, delim);
		data += ReencodeUtf8Range(bytes, raw_begin, close_quote + 1);
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

	// Returns the byte offset of the closing quote of ")delim"" at or
	// after search_from. The delimiter is matched by decoded code point,
	// not byte-wise: a stray Windows-1252 byte and its UTF-8 spelling
	// are the same d-char (reference behavior).
	size_t FindRawClosingQuote(size_t search_from,
	                           const std::vector<int>& delim) const
	{
		const std::string& bytes = source_.bytes;
		size_t r = search_from;
		while (r < bytes.size())
		{
			if (DecodeUtf8Char(bytes, r) != ')')
				continue;
			size_t q = r;
			bool match = true;
			for (size_t k = 0; match && k < delim.size(); k++)
				match = q < bytes.size() &&
					DecodeUtf8Char(bytes, q) == delim[k];
			if (match && q < bytes.size() && bytes[q] == '"')
				return q;
		}
		throw std::runtime_error("unterminated raw string literal");
	}

	// First translated index at or after the given byte offset. src_begin
	// is nondecreasing across the translated stream.
	size_t ResumeIndexAfterRaw(size_t byte_offset) const
	{
		size_t lo = 0;
		size_t hi = Chars().size();
		while (lo < hi)
		{
			size_t mid = (lo + hi) / 2;
			if (Chars()[mid].src_begin < byte_offset)
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
		while (At(i) != '\n' && At(i) != kEndOfInputChar && At(i) != close)
			i++;
		if (At(i) != close)
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
