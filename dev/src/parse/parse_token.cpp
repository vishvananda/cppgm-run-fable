#include "parse_token.h"

#include <stdexcept>

using std::runtime_error;

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
				result.push_back(ParseToken(PTOK_RSHIFT_1, OP_RSHIFT, ">"));
				result.push_back(ParseToken(PTOK_RSHIFT_2, OP_RSHIFT, ">"));
			}
			else
				result.push_back(ParseToken(PTOK_SIMPLE, token.token_type,
				                            token.source));
			break;
		case PTK_IDENTIFIER:
			result.push_back(ParseToken(PTOK_IDENTIFIER, KW_ALIGNAS,
			                            token.source));
			break;
		case PTK_EOF:
			result.push_back(ParseToken(PTOK_EOF, KW_ALIGNAS, ""));
			break;
		default:
			// PTK_LITERAL, PTK_LITERAL_ARRAY, and the user-defined kinds
			// are all TT_LITERAL terminals in the PA6 grammar.
			result.push_back(ParseToken(PTOK_LITERAL, KW_ALIGNAS,
			                            token.source));
			break;
		}
	}
	if (result.empty() || result.back().kind != PTOK_EOF)
		result.push_back(ParseToken(PTOK_EOF, KW_ALIGNAS, ""));
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
