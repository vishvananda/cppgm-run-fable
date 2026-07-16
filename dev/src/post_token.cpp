#include "post_token.h"

#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

const std::map<EFundamentalType, string>& FundamentalTypeNames()
{
	static const std::map<EFundamentalType, string> names =
	{
		{FT_SIGNED_CHAR, "signed char"},
		{FT_SHORT_INT, "short int"},
		{FT_INT, "int"},
		{FT_LONG_INT, "long int"},
		{FT_LONG_LONG_INT, "long long int"},
		{FT_UNSIGNED_CHAR, "unsigned char"},
		{FT_UNSIGNED_SHORT_INT, "unsigned short int"},
		{FT_UNSIGNED_INT, "unsigned int"},
		{FT_UNSIGNED_LONG_INT, "unsigned long int"},
		{FT_UNSIGNED_LONG_LONG_INT, "unsigned long long int"},
		{FT_WCHAR_T, "wchar_t"},
		{FT_CHAR, "char"},
		{FT_CHAR16_T, "char16_t"},
		{FT_CHAR32_T, "char32_t"},
		{FT_BOOL, "bool"},
		{FT_FLOAT, "float"},
		{FT_DOUBLE, "double"},
		{FT_LONG_DOUBLE, "long double"},
		{FT_VOID, "void"},
		{FT_NULLPTR_T, "nullptr_t"},
		{FT_INT128, "__int128"},
		{FT_UINT128, "unsigned __int128"}
	};
	return names;
}

const std::unordered_map<string, ETokenType>& SimpleTokenTypes()
{
	static const std::unordered_map<string, ETokenType> types =
	{
		// keywords
		{"alignas", KW_ALIGNAS},
		{"alignof", KW_ALIGNOF},
		{"asm", KW_ASM},
		{"auto", KW_AUTO},
		{"bool", KW_BOOL},
		{"break", KW_BREAK},
		{"case", KW_CASE},
		{"catch", KW_CATCH},
		{"char", KW_CHAR},
		{"char16_t", KW_CHAR16_T},
		{"char32_t", KW_CHAR32_T},
		{"class", KW_CLASS},
		{"const", KW_CONST},
		{"constexpr", KW_CONSTEXPR},
		{"const_cast", KW_CONST_CAST},
		{"continue", KW_CONTINUE},
		{"decltype", KW_DECLTYPE},
		{"default", KW_DEFAULT},
		{"delete", KW_DELETE},
		{"do", KW_DO},
		{"double", KW_DOUBLE},
		{"dynamic_cast", KW_DYNAMIC_CAST},
		{"else", KW_ELSE},
		{"enum", KW_ENUM},
		{"explicit", KW_EXPLICIT},
		{"export", KW_EXPORT},
		{"extern", KW_EXTERN},
		{"false", KW_FALSE},
		{"float", KW_FLOAT},
		{"for", KW_FOR},
		{"friend", KW_FRIEND},
		{"goto", KW_GOTO},
		{"if", KW_IF},
		{"inline", KW_INLINE},
		{"int", KW_INT},
		{"long", KW_LONG},
		{"mutable", KW_MUTABLE},
		{"namespace", KW_NAMESPACE},
		{"new", KW_NEW},
		{"noexcept", KW_NOEXCEPT},
		{"nullptr", KW_NULLPTR},
		{"operator", KW_OPERATOR},
		{"private", KW_PRIVATE},
		{"protected", KW_PROTECTED},
		{"public", KW_PUBLIC},
		{"register", KW_REGISTER},
		{"reinterpret_cast", KW_REINTERPET_CAST},
		{"return", KW_RETURN},
		{"short", KW_SHORT},
		{"signed", KW_SIGNED},
		{"sizeof", KW_SIZEOF},
		{"static", KW_STATIC},
		{"static_assert", KW_STATIC_ASSERT},
		{"static_cast", KW_STATIC_CAST},
		{"struct", KW_STRUCT},
		{"switch", KW_SWITCH},
		{"template", KW_TEMPLATE},
		{"this", KW_THIS},
		{"thread_local", KW_THREAD_LOCAL},
		{"throw", KW_THROW},
		{"true", KW_TRUE},
		{"try", KW_TRY},
		{"typedef", KW_TYPEDEF},
		{"typeid", KW_TYPEID},
		{"typename", KW_TYPENAME},
		{"union", KW_UNION},
		{"unsigned", KW_UNSIGNED},
		{"using", KW_USING},
		{"virtual", KW_VIRTUAL},
		{"void", KW_VOID},
		{"volatile", KW_VOLATILE},
		{"wchar_t", KW_WCHAR_T},
		{"while", KW_WHILE},

		// operators/punctuation
		{"{", OP_LBRACE},
		{"<%", OP_LBRACE},
		{"}", OP_RBRACE},
		{"%>", OP_RBRACE},
		{"[", OP_LSQUARE},
		{"<:", OP_LSQUARE},
		{"]", OP_RSQUARE},
		{":>", OP_RSQUARE},
		{"(", OP_LPAREN},
		{")", OP_RPAREN},
		{"|", OP_BOR},
		{"bitor", OP_BOR},
		{"^", OP_XOR},
		{"xor", OP_XOR},
		{"~", OP_COMPL},
		{"compl", OP_COMPL},
		{"&", OP_AMP},
		{"bitand", OP_AMP},
		{"!", OP_LNOT},
		{"not", OP_LNOT},
		{";", OP_SEMICOLON},
		{":", OP_COLON},
		{"...", OP_DOTS},
		{"?", OP_QMARK},
		{"::", OP_COLON2},
		{".", OP_DOT},
		{".*", OP_DOTSTAR},
		{"+", OP_PLUS},
		{"-", OP_MINUS},
		{"*", OP_STAR},
		{"/", OP_DIV},
		{"%", OP_MOD},
		{"=", OP_ASS},
		{"<", OP_LT},
		{">", OP_GT},
		{"+=", OP_PLUSASS},
		{"-=", OP_MINUSASS},
		{"*=", OP_STARASS},
		{"/=", OP_DIVASS},
		{"%=", OP_MODASS},
		{"^=", OP_XORASS},
		{"xor_eq", OP_XORASS},
		{"&=", OP_BANDASS},
		{"and_eq", OP_BANDASS},
		{"|=", OP_BORASS},
		{"or_eq", OP_BORASS},
		{"<<", OP_LSHIFT},
		{">>", OP_RSHIFT},
		{">>=", OP_RSHIFTASS},
		{"<<=", OP_LSHIFTASS},
		{"==", OP_EQ},
		{"!=", OP_NE},
		{"not_eq", OP_NE},
		{"<=", OP_LE},
		{">=", OP_GE},
		{"&&", OP_LAND},
		{"and", OP_LAND},
		{"||", OP_LOR},
		{"or", OP_LOR},
		{"++", OP_INC},
		{"--", OP_DEC},
		{",", OP_COMMA},
		{"->*", OP_ARROWSTAR},
		{"->", OP_ARROW}
	};
	return types;
}

const std::map<ETokenType, string>& TokenTypeNames()
{
	static const std::map<ETokenType, string> names =
	{
		{KW_ALIGNAS, "KW_ALIGNAS"},
		{KW_ALIGNOF, "KW_ALIGNOF"},
		{KW_ASM, "KW_ASM"},
		{KW_AUTO, "KW_AUTO"},
		{KW_BOOL, "KW_BOOL"},
		{KW_BREAK, "KW_BREAK"},
		{KW_CASE, "KW_CASE"},
		{KW_CATCH, "KW_CATCH"},
		{KW_CHAR, "KW_CHAR"},
		{KW_CHAR16_T, "KW_CHAR16_T"},
		{KW_CHAR32_T, "KW_CHAR32_T"},
		{KW_CLASS, "KW_CLASS"},
		{KW_CONST, "KW_CONST"},
		{KW_CONSTEXPR, "KW_CONSTEXPR"},
		{KW_CONST_CAST, "KW_CONST_CAST"},
		{KW_CONTINUE, "KW_CONTINUE"},
		{KW_DECLTYPE, "KW_DECLTYPE"},
		{KW_DEFAULT, "KW_DEFAULT"},
		{KW_DELETE, "KW_DELETE"},
		{KW_DO, "KW_DO"},
		{KW_DOUBLE, "KW_DOUBLE"},
		{KW_DYNAMIC_CAST, "KW_DYNAMIC_CAST"},
		{KW_ELSE, "KW_ELSE"},
		{KW_ENUM, "KW_ENUM"},
		{KW_EXPLICIT, "KW_EXPLICIT"},
		{KW_EXPORT, "KW_EXPORT"},
		{KW_EXTERN, "KW_EXTERN"},
		{KW_FALSE, "KW_FALSE"},
		{KW_FLOAT, "KW_FLOAT"},
		{KW_FOR, "KW_FOR"},
		{KW_FRIEND, "KW_FRIEND"},
		{KW_GOTO, "KW_GOTO"},
		{KW_IF, "KW_IF"},
		{KW_INLINE, "KW_INLINE"},
		{KW_INT, "KW_INT"},
		{KW_LONG, "KW_LONG"},
		{KW_MUTABLE, "KW_MUTABLE"},
		{KW_NAMESPACE, "KW_NAMESPACE"},
		{KW_NEW, "KW_NEW"},
		{KW_NOEXCEPT, "KW_NOEXCEPT"},
		{KW_NULLPTR, "KW_NULLPTR"},
		{KW_OPERATOR, "KW_OPERATOR"},
		{KW_PRIVATE, "KW_PRIVATE"},
		{KW_PROTECTED, "KW_PROTECTED"},
		{KW_PUBLIC, "KW_PUBLIC"},
		{KW_REGISTER, "KW_REGISTER"},
		{KW_REINTERPET_CAST, "KW_REINTERPET_CAST"},
		{KW_RETURN, "KW_RETURN"},
		{KW_SHORT, "KW_SHORT"},
		{KW_SIGNED, "KW_SIGNED"},
		{KW_SIZEOF, "KW_SIZEOF"},
		{KW_STATIC, "KW_STATIC"},
		{KW_STATIC_ASSERT, "KW_STATIC_ASSERT"},
		{KW_STATIC_CAST, "KW_STATIC_CAST"},
		{KW_STRUCT, "KW_STRUCT"},
		{KW_SWITCH, "KW_SWITCH"},
		{KW_TEMPLATE, "KW_TEMPLATE"},
		{KW_THIS, "KW_THIS"},
		{KW_THREAD_LOCAL, "KW_THREAD_LOCAL"},
		{KW_THROW, "KW_THROW"},
		{KW_TRUE, "KW_TRUE"},
		{KW_TRY, "KW_TRY"},
		{KW_TYPEDEF, "KW_TYPEDEF"},
		{KW_TYPEID, "KW_TYPEID"},
		{KW_TYPENAME, "KW_TYPENAME"},
		{KW_UNION, "KW_UNION"},
		{KW_UNSIGNED, "KW_UNSIGNED"},
		{KW_USING, "KW_USING"},
		{KW_VIRTUAL, "KW_VIRTUAL"},
		{KW_VOID, "KW_VOID"},
		{KW_VOLATILE, "KW_VOLATILE"},
		{KW_WCHAR_T, "KW_WCHAR_T"},
		{KW_WHILE, "KW_WHILE"},
		{OP_LBRACE, "OP_LBRACE"},
		{OP_RBRACE, "OP_RBRACE"},
		{OP_LSQUARE, "OP_LSQUARE"},
		{OP_RSQUARE, "OP_RSQUARE"},
		{OP_LPAREN, "OP_LPAREN"},
		{OP_RPAREN, "OP_RPAREN"},
		{OP_BOR, "OP_BOR"},
		{OP_XOR, "OP_XOR"},
		{OP_COMPL, "OP_COMPL"},
		{OP_AMP, "OP_AMP"},
		{OP_LNOT, "OP_LNOT"},
		{OP_SEMICOLON, "OP_SEMICOLON"},
		{OP_COLON, "OP_COLON"},
		{OP_DOTS, "OP_DOTS"},
		{OP_QMARK, "OP_QMARK"},
		{OP_COLON2, "OP_COLON2"},
		{OP_DOT, "OP_DOT"},
		{OP_DOTSTAR, "OP_DOTSTAR"},
		{OP_PLUS, "OP_PLUS"},
		{OP_MINUS, "OP_MINUS"},
		{OP_STAR, "OP_STAR"},
		{OP_DIV, "OP_DIV"},
		{OP_MOD, "OP_MOD"},
		{OP_ASS, "OP_ASS"},
		{OP_LT, "OP_LT"},
		{OP_GT, "OP_GT"},
		{OP_PLUSASS, "OP_PLUSASS"},
		{OP_MINUSASS, "OP_MINUSASS"},
		{OP_STARASS, "OP_STARASS"},
		{OP_DIVASS, "OP_DIVASS"},
		{OP_MODASS, "OP_MODASS"},
		{OP_XORASS, "OP_XORASS"},
		{OP_BANDASS, "OP_BANDASS"},
		{OP_BORASS, "OP_BORASS"},
		{OP_LSHIFT, "OP_LSHIFT"},
		{OP_RSHIFT, "OP_RSHIFT"},
		{OP_RSHIFTASS, "OP_RSHIFTASS"},
		{OP_LSHIFTASS, "OP_LSHIFTASS"},
		{OP_EQ, "OP_EQ"},
		{OP_NE, "OP_NE"},
		{OP_LE, "OP_LE"},
		{OP_GE, "OP_GE"},
		{OP_LAND, "OP_LAND"},
		{OP_LOR, "OP_LOR"},
		{OP_INC, "OP_INC"},
		{OP_DEC, "OP_DEC"},
		{OP_COMMA, "OP_COMMA"},
		{OP_ARROWSTAR, "OP_ARROWSTAR"},
		{OP_ARROW, "OP_ARROW"}
	};
	return names;
}

char ValueToHexChar(int c)
{
	if (c >= 0 && c <= 9)
		return '0' + c;
	if (c >= 10 && c <= 15)
		return 'A' + (c - 10);
	throw std::logic_error("ValueToHexChar of nonhex value");
}

string HexDump(const string& data)
{
	string s(data.size() * 2, '?');
	for (size_t i = 0; i < data.size(); i++)
	{
		unsigned char byte = data[i];
		s[2 * i + 0] = ValueToHexChar((byte & 0xF0) >> 4);
		s[2 * i + 1] = ValueToHexChar(byte & 0x0F);
	}
	return s;
}

void DescribeLiteralValue(const PostToken& token, std::ostringstream& out)
{
	if (token.kind == PTK_LITERAL_ARRAY || token.kind == PTK_UD_STRING)
		out << "array of " << token.num_elements << " ";
	out << FundamentalTypeName(token.type) << " " << HexDump(token.data);
}

} // namespace

PostToken MakeInvalidToken(const string& source)
{
	PostToken token;
	token.kind = PTK_INVALID;
	token.source = source;
	return token;
}

string LittleEndianBytes(unsigned long long value, size_t size)
{
	string bytes(size, '\0');
	for (size_t i = 0; i < size; i++)
		bytes[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
	return bytes;
}

unsigned long long LittleEndianValue(const string& bytes)
{
	unsigned long long value = 0;
	for (size_t i = bytes.size(); i-- > 0;)
		value = (value << 8) | static_cast<unsigned char>(bytes[i]);
	return value;
}

bool LookupSimpleTokenType(const string& spelling, ETokenType& type)
{
	auto it = SimpleTokenTypes().find(spelling);
	if (it == SimpleTokenTypes().end())
		return false;
	type = it->second;
	return true;
}

const string& TokenTypeName(ETokenType type)
{
	return TokenTypeNames().at(type);
}

const string& FundamentalTypeName(EFundamentalType type)
{
	return FundamentalTypeNames().at(type);
}

string DescribePostToken(const PostToken& token)
{
	std::ostringstream out;
	switch (token.kind)
	{
	case PTK_INVALID:
		out << "invalid " << token.source;
		break;
	case PTK_SIMPLE:
		out << "simple " << token.source << " "
		    << TokenTypeName(token.token_type);
		break;
	case PTK_IDENTIFIER:
		out << "identifier " << token.source;
		break;
	case PTK_LITERAL:
	case PTK_LITERAL_ARRAY:
		out << "literal " << token.source << " ";
		DescribeLiteralValue(token, out);
		break;
	case PTK_UD_INTEGER:
		out << "user-defined-literal " << token.source << " "
		    << token.ud_suffix << " integer " << token.ud_prefix;
		break;
	case PTK_UD_FLOATING:
		out << "user-defined-literal " << token.source << " "
		    << token.ud_suffix << " floating " << token.ud_prefix;
		break;
	case PTK_UD_CHARACTER:
		out << "user-defined-literal " << token.source << " "
		    << token.ud_suffix << " character ";
		DescribeLiteralValue(token, out);
		break;
	case PTK_UD_STRING:
		out << "user-defined-literal " << token.source << " "
		    << token.ud_suffix << " string ";
		DescribeLiteralValue(token, out);
		break;
	case PTK_EOF:
		out << "eof";
		break;
	}
	return out.str();
}
