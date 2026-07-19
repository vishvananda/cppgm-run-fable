#include "numeric_literals.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lex_char_classes.h"

namespace {

// 2.14.2: unsigned-suffix and long/long-long-suffix in either order; the
// two characters of a long-long-suffix must match case.
struct IntegerSuffix
{
	bool is_unsigned;
	int long_rank;  // 0 plain, 1 long, 2 long long
};

// Matches s[pos..end) against integer-suffix (or nothing).
bool MatchIntegerSuffix(const string& s, size_t pos, IntegerSuffix& suffix)
{
	suffix.is_unsigned = false;
	suffix.long_rank = 0;
	if (pos < s.size() && (s[pos] == 'u' || s[pos] == 'U'))
	{
		suffix.is_unsigned = true;
		pos++;
	}
	if (pos < s.size() && (s[pos] == 'l' || s[pos] == 'L'))
	{
		char first = s[pos++];
		suffix.long_rank = 1;
		if (pos < s.size() && s[pos] == first)
		{
			pos++;
			suffix.long_rank = 2;
		}
		if (!suffix.is_unsigned && pos < s.size() &&
		    (s[pos] == 'u' || s[pos] == 'U'))
		{
			suffix.is_unsigned = true;
			pos++;
		}
	}
	return pos == s.size();
}

// Matches a decimal-literal, octal-literal, hexadecimal-literal, or
// binary-literal (GNU 0b extension, accepted by the reference) at the
// start of s; end is set one past the last digit.
bool MatchIntegerDigits(const string& s, size_t& end, int& base)
{
	if (s.empty() || !IsDigit(s[0]))
		return false;
	if (s[0] != '0')
	{
		size_t i = 1;
		while (i < s.size() && IsDigit(s[i]))
			i++;
		base = 10;
		end = i;
		return true;
	}
	if (s.size() >= 2 && (s[1] == 'x' || s[1] == 'X'))
	{
		size_t i = 2;
		while (i < s.size() && IsHexDigit(s[i]))
			i++;
		if (i == 2)
			return false;
		base = 16;
		end = i;
		return true;
	}
	if (s.size() >= 2 && (s[1] == 'b' || s[1] == 'B'))
	{
		size_t i = 2;
		while (i < s.size() && (s[i] == '0' || s[i] == '1'))
			i++;
		if (i == 2)
			return false;
		base = 2;
		end = i;
		return true;
	}
	size_t i = 1;
	while (i < s.size() && IsOctalDigit(s[i]))
		i++;
	base = 8;
	end = i;
	return true;
}

// Accumulates the digit range into value; false on overflow of the
// widest integer type.
bool ComputeIntegerValue(const string& s, size_t end, int base,
                         unsigned long long& value)
{
	value = 0;
	size_t pos = base == 16 || base == 2 ? 2 : 0;
	for (; pos < end; pos++)
	{
		unsigned long long digit = HexDigitValue(s[pos]);
		if (value > (ULLONG_MAX - digit) / base)
			return false;
		value = value * base + digit;
	}
	return true;
}

struct IntegerType
{
	EFundamentalType type;
	unsigned long long max;
	size_t size;
};

// 2.14.2 Table 6 on the LP64 ABI: the candidate list the literal's value
// is matched against in order. Decimal literals without an
// unsigned-suffix never fall back to unsigned types.
std::vector<IntegerType> IntegerTypeCandidates(const IntegerSuffix& suffix,
                                               bool decimal)
{
	const IntegerType int_t = {FT_INT, INT_MAX, 4};
	const IntegerType uint_t = {FT_UNSIGNED_INT, UINT_MAX, 4};
	const IntegerType long_t = {FT_LONG_INT, LONG_MAX, 8};
	const IntegerType ulong_t = {FT_UNSIGNED_LONG_INT, ULONG_MAX, 8};
	const IntegerType llong_t = {FT_LONG_LONG_INT, LLONG_MAX, 8};
	const IntegerType ullong_t = {FT_UNSIGNED_LONG_LONG_INT, ULLONG_MAX, 8};

	std::vector<IntegerType> candidates;
	if (suffix.is_unsigned)
	{
		if (suffix.long_rank == 0)
			candidates.push_back(uint_t);
		if (suffix.long_rank <= 1)
			candidates.push_back(ulong_t);
		candidates.push_back(ullong_t);
	}
	else if (decimal)
	{
		if (suffix.long_rank == 0)
			candidates.push_back(int_t);
		if (suffix.long_rank <= 1)
			candidates.push_back(long_t);
		candidates.push_back(llong_t);
	}
	else
	{
		if (suffix.long_rank == 0)
		{
			candidates.push_back(int_t);
			candidates.push_back(uint_t);
		}
		if (suffix.long_rank <= 1)
		{
			candidates.push_back(long_t);
			candidates.push_back(ulong_t);
		}
		candidates.push_back(llong_t);
		candidates.push_back(ullong_t);
	}
	return candidates;
}

PostToken MakeIntegerLiteral(const string& source, size_t digits_end,
                             int base, const IntegerSuffix& suffix)
{
	unsigned long long value;
	if (!ComputeIntegerValue(source, digits_end, base, value))
		return MakeInvalidToken(source);
	for (const IntegerType& candidate :
	     IntegerTypeCandidates(suffix, base == 10))
	{
		if (value > candidate.max)
			continue;
		PostToken token;
		token.kind = PTK_LITERAL;
		token.source = source;
		token.type = candidate.type;
		token.data = LittleEndianBytes(value, candidate.size);
		return token;
	}
	return MakeInvalidToken(source);
}

// The floating-suffix tail: the basic f/l set, plus the GNU/C23
// extended forms hosted headers use (q/Q/w/W and the _FloatN family
// bf16/f16/f32/f64/f128 with x variants), each mapped to the nearest
// evaluated type: 'f' float, 0 double, 'q' long double.
bool MatchGnuFloatingSuffix(const string& tail, char& suffix)
{
	suffix = 0;
	if (tail.empty())
		return true;
	if (tail.size() == 1)
	{
		char c = tail[0];
		if (c == 'f' || c == 'F' || c == 'l' || c == 'L' ||
		    c == 'q' || c == 'Q')
		{
			suffix = c;
			return true;
		}
		if (c == 'w' || c == 'W')
		{
			suffix = 'q';
			return true;
		}
		return false;
	}
	string lower;
	for (size_t i = 0; i < tail.size(); i++)
		lower += (char)std::tolower((unsigned char)tail[i]);
	if (lower == "f16" || lower == "bf16" || lower == "f32")
	{
		suffix = 'f';
		return true;
	}
	if (lower == "f64" || lower == "f32x")
		return true;
	if (lower == "f128" || lower == "f64x")
	{
		suffix = 'q';
		return true;
	}
	return false;
}

// Matches floating-literal (2.14.4); suffix is the floating-suffix
// character or 0 when absent.
bool MatchFloating(const string& s, char& suffix)
{
	size_t i = 0;
	size_t int_digits = 0;
	size_t frac_digits = 0;
	while (i < s.size() && IsDigit(s[i]))
	{
		i++;
		int_digits++;
	}
	bool has_dot = i < s.size() && s[i] == '.';
	if (has_dot)
	{
		i++;
		while (i < s.size() && IsDigit(s[i]))
		{
			i++;
			frac_digits++;
		}
	}
	if (int_digits + frac_digits == 0)
		return false;
	bool has_exponent = i < s.size() && (s[i] == 'e' || s[i] == 'E');
	if (has_exponent)
	{
		i++;
		if (i < s.size() && (s[i] == '+' || s[i] == '-'))
			i++;
		size_t exponent_digits = 0;
		while (i < s.size() && IsDigit(s[i]))
		{
			i++;
			exponent_digits++;
		}
		if (exponent_digits == 0)
			return false;
	}
	// fractional-constant requires the dot; a plain digit-sequence
	// requires the exponent-part
	if (!has_dot && !has_exponent)
		return false;
	return MatchGnuFloatingSuffix(s.substr(i), suffix);
}

// The reference toolchain additionally accepts C99-style hexadecimal
// floating literals: 0x, hex digits with an optional fraction, a p/P
// exponent with a required decimal digit-sequence, and only l/L (long
// double) as an optional floating-suffix.
bool MatchHexFloating(const string& s, char& suffix)
{
	if (s.size() < 2 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
		return false;
	size_t i = 2;
	size_t digits = 0;
	while (i < s.size() && IsHexDigit(s[i]))
	{
		i++;
		digits++;
	}
	if (i < s.size() && s[i] == '.')
	{
		i++;
		while (i < s.size() && IsHexDigit(s[i]))
		{
			i++;
			digits++;
		}
	}
	if (digits == 0 || i >= s.size() || (s[i] != 'p' && s[i] != 'P'))
		return false;
	i++;
	if (i < s.size() && (s[i] == '+' || s[i] == '-'))
		i++;
	size_t exponent_digits = 0;
	while (i < s.size() && IsDigit(s[i]))
	{
		i++;
		exponent_digits++;
	}
	if (exponent_digits == 0)
		return false;
	suffix = 0;
	if (i < s.size() &&
	    (s[i] == 'l' || s[i] == 'L' || s[i] == 'q' || s[i] == 'Q'))
		suffix = s[i++];
	return i == s.size();
}

// Packs the low value_size bytes of the scalar at value into an
// object_size-byte zero-padded object representation (the x87 long
// double has 10 value bytes inside a 16-byte object).
string ValueObjectBytes(const void* value, size_t value_size,
                        size_t object_size)
{
	string bytes(object_size, '\0');
	std::memcpy(&bytes[0], value, value_size);
	return bytes;
}

// Scans the floating literal the way the reference does: its era of
// libstdc++ num_get delegated to strtof/strtod/strtold and stored the
// result directly, so an out-of-range literal reads as +/-infinity (or
// zero on underflow) where the modern extraction would clamp to the
// largest finite value. strtod also accepts the C99 hexadecimal forms
// that istringstream cannot parse.
PostToken MakeFloatingLiteral(const string& source, char suffix)
{
	string spelling =
		suffix ? source.substr(0, source.size() - 1) : source;
	PostToken token;
	token.kind = PTK_LITERAL;
	token.source = source;
	if (suffix == 'f' || suffix == 'F')
	{
		float x = std::strtof(spelling.c_str(), nullptr);
		token.type = FT_FLOAT;
		token.data = ValueObjectBytes(&x, sizeof(x), sizeof(x));
	}
	else if (suffix == 'l' || suffix == 'L' ||
	         suffix == 'q' || suffix == 'Q')
	{
		// PA34: the GNU __float128 suffix arrives through hosted headers;
		// this stage evaluates it at long double precision.
		long double x = std::strtold(spelling.c_str(), nullptr);
		token.type = FT_LONG_DOUBLE;
		token.data = ValueObjectBytes(&x, 10, 16);
	}
	else
	{
		double x = std::strtod(spelling.c_str(), nullptr);
		token.type = FT_DOUBLE;
		token.data = ValueObjectBytes(&x, sizeof(x), sizeof(x));
	}
	return token;
}

enum EUdNumberKind
{
	UD_NUMBER_INVALID,
	UD_NUMBER_INTEGER,
	UD_NUMBER_FLOATING
};

// Shape scanner for a pp-number that contains an underscore, replicating
// the reference toolchain: its number scanner keeps running across the
// ud-suffix, so an exponent marker ([eE], or [pP] once hex) immediately
// followed by a decimal digit counts as the literal's exponent even
// inside the suffix. Consequences pinned by the reference: 123_e3 is
// ill-formed while 123_ex is fine; an x/X anywhere in the suffix
// switches to hex interpretation and changes the marker (123_xe3 valid,
// 123_xp3 invalid, 1e_xp3 floating); a dangling exponent marker must
// complete somewhere inside the suffix, hunting across arbitrary
// characters (1e_e3, 5E_bhE8, 1e_8e3 are floating; 1e_x and 1e_ea3 are
// ill-formed); a dot may follow a completed exponent (1e3.4_x);
// octality is enforced only if the number stays an integer (078_x
// invalid, 078e2_x floating).
class UdNumberShapeScanner
{
public:
	UdNumberShapeScanner()
		: state_(kInt), hex_(false), binary_(false), octal_(false),
		  dot_seen_(false), bad_octal_(false), mantissa_digits_(0)
	{}

	EUdNumberKind Scan(const string& s);

private:
	enum State
	{
		kInt,         // integer digits
		kFrac,        // seen '.', taking fraction digits
		kExpMark,     // exponent marker without sign or digits yet
		kExpSign,     // marker and sign; digits required
		kExpDigits,   // exponent complete
		kExpFrac,     // '.' after a complete exponent
		kSufInt,      // suffix of an integer
		kSufIntMark,  // exponent marker inside an integer suffix
		kSufFloat,    // suffix of a floating number: inert
		kSufHunt,     // suffix hunting the dangling exponent
		kSufHuntMark  // marker repeated inside the hunting suffix
	};

	bool IsMarker(char c) const
	{
		return hex_ ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E');
	}

	bool IsNumberDigit(char c) const
	{
		return hex_ ? IsHexDigit(c) : IsDigit(c);
	}

	// Any character a pp-number ud-suffix may contain: bytes at or
	// above 0x80 belong to identifier-continue code points (the PA1
	// tokenizer validated them).
	static bool IsSuffixChar(char c)
	{
		return IsDigit(c) || IsNondigit(c) ||
			static_cast<unsigned char>(c) >= 0x80;
	}

	bool StepNumber(char c);
	bool StepSuffix(char c);

	State state_;
	bool hex_;
	bool binary_;
	bool octal_;
	bool dot_seen_;
	bool bad_octal_;
	size_t mantissa_digits_;
};

bool UdNumberShapeScanner::StepNumber(char c)
{
	switch (state_)
	{
	case kInt:
		// a binary literal (GNU 0b extension, accepted by the reference)
		// takes only binary digits and a ud-suffix: no dot, no exponent
		if (binary_)
		{
			if (c == '0' || c == '1')
			{
				mantissa_digits_++;
				return true;
			}
			if (c == '_' && mantissa_digits_ > 0)
			{
				state_ = kSufInt;
				return true;
			}
			return false;
		}
		if (IsNumberDigit(c))
		{
			if (octal_ && c > '7')
				bad_octal_ = true;
			mantissa_digits_++;
			return true;
		}
		if (c == '.')
		{
			dot_seen_ = true;
			state_ = kFrac;
			return true;
		}
		if (IsMarker(c))
		{
			state_ = kExpMark;
			return true;
		}
		if (c == '_' && mantissa_digits_ > 0 && !bad_octal_)
		{
			state_ = kSufInt;
			return true;
		}
		return false;
	case kFrac:
		if (IsNumberDigit(c))
		{
			mantissa_digits_++;
			return true;
		}
		if (IsMarker(c))
		{
			state_ = kExpMark;
			return true;
		}
		// a hex fraction is floating only once its exponent appears
		if (c == '_' && mantissa_digits_ > 0 && !hex_)
		{
			state_ = kSufFloat;
			return true;
		}
		return false;
	case kExpMark:
		if (IsDigit(c))  // the first exponent digit must be decimal
		{
			state_ = kExpDigits;
			return true;
		}
		if (c == '+' || c == '-')
		{
			state_ = kExpSign;
			return true;
		}
		if (c == '.' && !dot_seen_)
		{
			dot_seen_ = true;
			state_ = kExpFrac;
			return true;
		}
		if (c == '_' && mantissa_digits_ > 0)
		{
			state_ = dot_seen_ ? kSufFloat : kSufHunt;
			return true;
		}
		return false;
	case kExpSign:
		// further exponent characters follow the literal's base
		// (0x1p+c is a complete hex exponent, 1e+f is not)
		if (IsNumberDigit(c))
		{
			state_ = kExpDigits;
			return true;
		}
		if (c == '.' && !dot_seen_)
		{
			dot_seen_ = true;
			state_ = kExpFrac;
			return true;
		}
		return false;
	case kExpDigits:
		if (IsNumberDigit(c))
			return true;
		if (c == '.' && !dot_seen_)
		{
			dot_seen_ = true;
			state_ = kExpFrac;
			return true;
		}
		if (c == '_')
		{
			state_ = kSufFloat;
			return true;
		}
		return false;
	default:  // kExpFrac
		if (IsNumberDigit(c))
			return true;
		if (c == '_')
		{
			state_ = kSufFloat;
			return true;
		}
		return false;
	}
}

bool UdNumberShapeScanner::StepSuffix(char c)
{
	switch (state_)
	{
	case kSufInt:
		if (c == 'x' || c == 'X')
		{
			hex_ = true;
			return true;
		}
		if (IsMarker(c))
		{
			state_ = kSufIntMark;
			return true;
		}
		return IsSuffixChar(c);
	case kSufIntMark:
		if (IsDigit(c))
			return false;  // the marker would form an exponent
		if (c == 'x' || c == 'X')
		{
			hex_ = true;
			state_ = kSufInt;
			return true;
		}
		if (IsMarker(c))
			return true;
		if (IsSuffixChar(c))
		{
			state_ = kSufInt;
			return true;
		}
		return false;
	case kSufFloat:
		return IsSuffixChar(c);
	case kSufHunt:
		if (c == 'x' || c == 'X')
		{
			hex_ = true;
			return true;
		}
		if (IsMarker(c))
		{
			state_ = kSufHuntMark;
			return true;
		}
		return IsSuffixChar(c);
	default:  // kSufHuntMark
		if (IsDigit(c))
		{
			state_ = kSufFloat;
			return true;
		}
		if (c == 'x' || c == 'X')
		{
			hex_ = true;
			state_ = kSufHunt;
			return true;
		}
		if (IsMarker(c))
			return true;
		if (IsSuffixChar(c))
		{
			state_ = kSufHunt;
			return true;
		}
		return false;
	}
}

EUdNumberKind UdNumberShapeScanner::Scan(const string& s)
{
	size_t i = 0;
	if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		hex_ = true;
		i = 2;
	}
	else if (s.size() >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
	{
		binary_ = true;
		i = 2;
	}
	else
	{
		octal_ = !s.empty() && s[0] == '0';
	}
	for (; i < s.size(); i++)
	{
		bool ok = state_ < kSufInt ? StepNumber(s[i]) : StepSuffix(s[i]);
		if (!ok)
			return UD_NUMBER_INVALID;
	}
	switch (state_)
	{
	case kSufInt:
	case kSufIntMark:
		return UD_NUMBER_INTEGER;
	case kSufFloat:
		return UD_NUMBER_FLOATING;
	default:
		return UD_NUMBER_INVALID;
	}
}

} // namespace

PostToken AnalyzePPNumber(const string& source)
{
	// A ud-suffix begins with an underscore (course rule per the
	// 17.6.4.3.5 reservation), so the first underscore separates the
	// numeric literal from the suffix for output purposes; validity
	// follows the reference shape scan.
	size_t underscore = source.find('_');
	if (underscore != string::npos)
	{
		EUdNumberKind kind = UdNumberShapeScanner().Scan(source);
		if (kind == UD_NUMBER_INVALID)
			return MakeInvalidToken(source);
		PostToken token;
		token.kind = kind == UD_NUMBER_INTEGER ? PTK_UD_INTEGER
		                                       : PTK_UD_FLOATING;
		token.source = source;
		token.ud_suffix = source.substr(underscore);
		token.ud_prefix = source.substr(0, underscore);
		return token;
	}

	size_t digits_end;
	int base;
	IntegerSuffix suffix;
	if (MatchIntegerDigits(source, digits_end, base) &&
	    MatchIntegerSuffix(source, digits_end, suffix))
		return MakeIntegerLiteral(source, digits_end, base, suffix);

	char floating_suffix;
	if (MatchFloating(source, floating_suffix) ||
	    MatchHexFloating(source, floating_suffix))
		return MakeFloatingLiteral(source, floating_suffix);

	return MakeInvalidToken(source);
}
