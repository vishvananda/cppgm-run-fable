#include "lowir/lowir_lexer.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

using std::runtime_error;

namespace {

bool is_name_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$';
}

bool is_number_char(char c)
{
	return isalnum((unsigned char)c) || c == '.' || c == '_';
}

struct Lexer
{
	const string & text;
	size_t pos = 0;
	long line = 1;

	explicit Lexer(const string & t) : text(t) {}

	bool done() const { return pos >= text.size(); }
	char peek(size_t ahead = 0) const
	{
		return pos + ahead < text.size() ? text[pos + ahead] : '\0';
	}

	void skip_space()
	{
		while(!done())
		{
			char c = text[pos];
			if(c == '\n')
			{
				++line;
				++pos;
			}
			else if(c == ' ' || c == '\t' || c == '\r' || c == '\v' ||
			        c == '\f')
				++pos;
			else
				break;
		}
	}

	LowIRToken make(ELowIRTokenKind kind, const string & spelling)
	{
		LowIRToken token;
		token.kind = kind;
		token.text = spelling;
		token.line = line;
		return token;
	}

	string take_while(bool (*pred)(char))
	{
		size_t start = pos;
		while(!done() && pred(text[pos]))
			++pos;
		return text.substr(start, pos - start);
	}

	LowIRToken lex_sigil_name(ELowIRTokenKind kind)
	{
		++pos;
		string name = take_while(is_name_char);
		if(name.empty())
			throw runtime_error("empty symbol name");
		return make(kind, name);
	}

	LowIRToken lex_identifier()
	{
		string word = take_while(is_name_char);
		// `obj<NxA>` is one type token so type parsing stays uniform.
		if(word == "obj" && peek() == '<')
		{
			size_t close = text.find('>', pos);
			if(close == string::npos)
				throw runtime_error("unterminated object type");
			word += text.substr(pos, close + 1 - pos);
			pos = close + 1;
		}
		return make(LOWIR_TOK_IDENTIFIER, word);
	}

	long lex_dbg_int()
	{
		skip_space();
		string digits = take_while(is_number_char);
		char * end = nullptr;
		long value = strtol(digits.c_str(), &end, 10);
		if(digits.empty() || *end != '\0' || value <= 0)
			throw runtime_error("bad debug location coordinate");
		return value;
	}

	void expect_char(char c, const char * what)
	{
		skip_space();
		if(done() || text[pos] != c)
			throw runtime_error(string("expected ") + what +
			                    " in debug location");
		++pos;
	}

	LowIRToken lex_debug_location()
	{
		LowIRToken token = make(LOWIR_TOK_DEBUG_LOC, "!dbg");
		++pos;
		string word = take_while(is_name_char);
		if(word != "dbg")
			throw runtime_error("unknown instruction suffix: !" + word);
		expect_char('(', "'('");
		skip_space();
		size_t start = pos;
		while(!done() && text[pos] != ',' && !isspace((unsigned char)text[pos]))
			++pos;
		token.dbg_file = text.substr(start, pos - start);
		if(token.dbg_file.empty())
			throw runtime_error("empty debug location file");
		expect_char(',', "','");
		token.dbg_line = lex_dbg_int();
		expect_char(',', "','");
		token.dbg_column = lex_dbg_int();
		expect_char(')', "')'");
		return token;
	}

	LowIRToken next()
	{
		skip_space();
		if(done())
			return make(LOWIR_TOK_EOF, "");
		char c = text[pos];
		if(c == '@')
			return lex_sigil_name(LOWIR_TOK_GLOBAL_NAME);
		if(c == '%')
			return lex_sigil_name(LOWIR_TOK_TEMP_NAME);
		if(c == '$')
			return lex_sigil_name(LOWIR_TOK_SLOT_NAME);
		if(c == '^')
			return lex_sigil_name(LOWIR_TOK_BLOCK_NAME);
		if(c == '!')
			return lex_debug_location();
		if(isdigit((unsigned char)c))
		{
			string digits = take_while(is_number_char);
			// signed exponents: "623e+100" scans as one number
			while((pos < text.size()) &&
			      (text[pos] == '+' || text[pos] == '-') &&
			      !digits.empty() &&
			      (digits[digits.size() - 1] == 'e' ||
			       digits[digits.size() - 1] == 'E') &&
			      pos + 1 < text.size() &&
			      isdigit((unsigned char)text[pos + 1]))
			{
				digits += text[pos];
				++pos;
				digits += take_while(is_number_char);
			}
			return make(LOWIR_TOK_NUMBER, digits);
		}
		if(isalpha((unsigned char)c) || c == '_')
			return lex_identifier();
		if(c == '-' && peek(1) == '>')
		{
			pos += 2;
			return make(LOWIR_TOK_PUNCT, "->");
		}
		if(string(":,=()[]{}<>+-").find(c) != string::npos)
		{
			++pos;
			return make(LOWIR_TOK_PUNCT, string(1, c));
		}
		throw runtime_error(string("stray character in LowIR input: ") + c);
	}
};

}  // namespace

vector<LowIRToken> LexLowIR(const string & text)
{
	Lexer lexer(text);
	vector<LowIRToken> tokens;
	for(;;)
	{
		LowIRToken token = lexer.next();
		bool at_end = token.kind == LOWIR_TOK_EOF;
		tokens.push_back(token);
		if(at_end)
			break;
	}
	return tokens;
}
