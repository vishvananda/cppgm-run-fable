#pragma once

#include <cstddef>
#include <string>

using std::size_t;
using std::string;

// See 3.9.1: Fundamental Types
enum EFundamentalType
{
	// 3.9.1.2
	FT_SIGNED_CHAR,
	FT_SHORT_INT,
	FT_INT,
	FT_LONG_INT,
	FT_LONG_LONG_INT,

	// 3.9.1.3
	FT_UNSIGNED_CHAR,
	FT_UNSIGNED_SHORT_INT,
	FT_UNSIGNED_INT,
	FT_UNSIGNED_LONG_INT,
	FT_UNSIGNED_LONG_LONG_INT,

	// 3.9.1.1 / 3.9.1.5
	FT_WCHAR_T,
	FT_CHAR,
	FT_CHAR16_T,
	FT_CHAR32_T,

	// 3.9.1.6
	FT_BOOL,

	// 3.9.1.8
	FT_FLOAT,
	FT_DOUBLE,
	FT_LONG_DOUBLE,

	// 3.9.1.9
	FT_VOID,

	// 3.9.1.10
	FT_NULLPTR_T
};

// Vocabulary of `simple` tokens: keywords and operators/punctuation per
// the PA2 course table (2.12, 2.13).
enum ETokenType
{
	// keywords
	KW_ALIGNAS,
	KW_ALIGNOF,
	KW_ASM,
	KW_AUTO,
	KW_BOOL,
	KW_BREAK,
	KW_CASE,
	KW_CATCH,
	KW_CHAR,
	KW_CHAR16_T,
	KW_CHAR32_T,
	KW_CLASS,
	KW_CONST,
	KW_CONSTEXPR,
	KW_CONST_CAST,
	KW_CONTINUE,
	KW_DECLTYPE,
	KW_DEFAULT,
	KW_DELETE,
	KW_DO,
	KW_DOUBLE,
	KW_DYNAMIC_CAST,
	KW_ELSE,
	KW_ENUM,
	KW_EXPLICIT,
	KW_EXPORT,
	KW_EXTERN,
	KW_FALSE,
	KW_FLOAT,
	KW_FOR,
	KW_FRIEND,
	KW_GOTO,
	KW_IF,
	KW_INLINE,
	KW_INT,
	KW_LONG,
	KW_MUTABLE,
	KW_NAMESPACE,
	KW_NEW,
	KW_NOEXCEPT,
	KW_NULLPTR,
	KW_OPERATOR,
	KW_PRIVATE,
	KW_PROTECTED,
	KW_PUBLIC,
	KW_REGISTER,
	KW_REINTERPET_CAST,
	KW_RETURN,
	KW_SHORT,
	KW_SIGNED,
	KW_SIZEOF,
	KW_STATIC,
	KW_STATIC_ASSERT,
	KW_STATIC_CAST,
	KW_STRUCT,
	KW_SWITCH,
	KW_TEMPLATE,
	KW_THIS,
	KW_THREAD_LOCAL,
	KW_THROW,
	KW_TRUE,
	KW_TRY,
	KW_TYPEDEF,
	KW_TYPEID,
	KW_TYPENAME,
	KW_UNION,
	KW_UNSIGNED,
	KW_USING,
	KW_VIRTUAL,
	KW_VOID,
	KW_VOLATILE,
	KW_WCHAR_T,
	KW_WHILE,

	// operators/punctuation
	OP_LBRACE,
	OP_RBRACE,
	OP_LSQUARE,
	OP_RSQUARE,
	OP_LPAREN,
	OP_RPAREN,
	OP_BOR,
	OP_XOR,
	OP_COMPL,
	OP_AMP,
	OP_LNOT,
	OP_SEMICOLON,
	OP_COLON,
	OP_DOTS,
	OP_QMARK,
	OP_COLON2,
	OP_DOT,
	OP_DOTSTAR,
	OP_PLUS,
	OP_MINUS,
	OP_STAR,
	OP_DIV,
	OP_MOD,
	OP_ASS,
	OP_LT,
	OP_GT,
	OP_PLUSASS,
	OP_MINUSASS,
	OP_STARASS,
	OP_DIVASS,
	OP_MODASS,
	OP_XORASS,
	OP_BANDASS,
	OP_BORASS,
	OP_LSHIFT,
	OP_RSHIFT,
	OP_RSHIFTASS,
	OP_LSHIFTASS,
	OP_EQ,
	OP_NE,
	OP_LE,
	OP_GE,
	OP_LAND,
	OP_LOR,
	OP_INC,
	OP_DEC,
	OP_COMMA,
	OP_ARROWSTAR,
	OP_ARROW
};

enum EPostTokenKind
{
	PTK_INVALID,
	PTK_SIMPLE,
	PTK_IDENTIFIER,
	PTK_LITERAL,        // scalar literal: type + value bytes
	PTK_LITERAL_ARRAY,  // string literal: element type + count + bytes
	PTK_UD_INTEGER,
	PTK_UD_FLOATING,
	PTK_UD_CHARACTER,
	PTK_UD_STRING,
	PTK_EOF
};

// One token produced by the tokenization part of translation phase 7.
// `source` is the PA1 token data; for a concatenated string-literal
// sequence it is the space-joined list of the member sources. Value bytes
// in `data` are the Linux x86-64 ABI object representation
// (little-endian).
struct PostToken
{
	PostToken()
		: kind(PTK_INVALID), token_type(KW_ALIGNAS), type(FT_VOID),
		  num_elements(0)
	{}

	EPostTokenKind kind;
	string source;
	ETokenType token_type;  // PTK_SIMPLE
	EFundamentalType type;  // literal scalar / array element type
	size_t num_elements;    // PTK_LITERAL_ARRAY / PTK_UD_STRING
	string data;            // value bytes for literal kinds
	string ud_suffix;       // user-defined literal kinds
	string ud_prefix;       // PTK_UD_INTEGER / PTK_UD_FLOATING
};

struct IPostTokenStream
{
	virtual void emit(const PostToken& token) = 0;
	virtual ~IPostTokenStream() {}
};

PostToken MakeInvalidToken(const string& source);

// size bytes of value in the ABI byte order (little-endian).
string LittleEndianBytes(unsigned long long value, size_t size);

// True and sets type when spelling is a keyword, identifier-like
// operator, or operator/punctuator with a `simple` mapping. The four
// preprocessing-only operators (#, ##, %:, %:%:) have no mapping.
bool LookupSimpleTokenType(const string& spelling, ETokenType& type);

const string& TokenTypeName(ETokenType type);            // "KW_AUTO"
const string& FundamentalTypeName(EFundamentalType type);  // "long long int"

// Renders the canonical PA2 output line for the token (no newline).
string DescribePostToken(const PostToken& token);
