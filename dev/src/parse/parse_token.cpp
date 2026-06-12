#include "parse_token.h"

#include <stdexcept>

using std::runtime_error;

namespace {

unsigned IdentifierFlags(const string& spelling)
{
	unsigned flags = 0;
	if (IsMockClassName(spelling))
		flags |= PTF_CLASS_NAME;
	if (IsMockTemplateName(spelling))
		flags |= PTF_TEMPLATE_NAME;
	if (IsMockTypedefName(spelling))
		flags |= PTF_TYPEDEF_NAME;
	if (IsMockEnumName(spelling))
		flags |= PTF_ENUM_NAME;
	if (IsMockNamespaceName(spelling))
		flags |= PTF_NAMESPACE_NAME;
	if (spelling == "override")
		flags |= PTF_ST_OVERRIDE;
	else if (spelling == "final")
		flags |= PTF_ST_FINAL;
	return flags;
}

unsigned LiteralFlags(const string& spelling)
{
	if (spelling == "\"\"")
		return PTF_ST_EMPTYSTR;
	if (spelling == "0")
		return PTF_ST_ZERO;
	return 0;
}

} // namespace

vector<ParseToken> BuildParseTokens(const vector<PostToken>& tokens)
{
	vector<ParseToken> result;
	result.reserve(tokens.size() + 1);
	for (size_t i = 0; i < tokens.size(); i++)
	{
		const PostToken& token = tokens[i];
		switch (token.kind)
		{
		case PTK_INVALID:
			throw runtime_error("invalid token at phase 7: " + token.source);
		case PTK_SIMPLE:
			if (token.token_type == OP_RSHIFT)
			{
				result.push_back(ParseToken(PTOK_RSHIFT_1, OP_RSHIFT, ">", 0));
				result.push_back(ParseToken(PTOK_RSHIFT_2, OP_RSHIFT, ">", 0));
			}
			else
				result.push_back(ParseToken(PTOK_SIMPLE, token.token_type,
				                            token.source, 0));
			break;
		case PTK_IDENTIFIER:
			result.push_back(ParseToken(PTOK_IDENTIFIER, KW_ALIGNAS,
			                            token.source,
			                            IdentifierFlags(token.source)));
			break;
		case PTK_EOF:
			result.push_back(ParseToken(PTOK_EOF, KW_ALIGNAS, "", 0));
			break;
		default:
			// PTK_LITERAL, PTK_LITERAL_ARRAY, and the user-defined kinds
			// are all TT_LITERAL terminals in the PA6 grammar.
			result.push_back(ParseToken(PTOK_LITERAL, KW_ALIGNAS,
			                            token.source,
			                            LiteralFlags(token.source)));
			result.back().literal_kind = token.kind;
			result.back().literal_type = token.type;
			result.back().literal_elements = token.num_elements;
			result.back().literal_data = token.data;
			result.back().ud_suffix = token.ud_suffix;
			break;
		}
	}
	if (result.empty() || result.back().kind != PTOK_EOF)
		result.push_back(ParseToken(PTOK_EOF, KW_ALIGNAS, "", 0));
	return result;
}

bool IsMockClassName(const string& identifier)
{
	return identifier.find('C') != string::npos;
}

bool IsMockTemplateName(const string& identifier)
{
	return identifier.find('T') != string::npos;
}

bool IsMockTypedefName(const string& identifier)
{
	return identifier.find('Y') != string::npos;
}

bool IsMockEnumName(const string& identifier)
{
	return identifier.find('E') != string::npos;
}

bool IsMockNamespaceName(const string& identifier)
{
	return identifier.find('N') != string::npos;
}
