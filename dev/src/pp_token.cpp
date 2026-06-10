#include "pp_token.h"

void PPTokenCollector::Add(EPPTokenKind kind, const string& data)
{
	PPToken token(kind, data);
	token.ws_before = pending_ws_;
	pending_ws_ = false;
	tokens.push_back(token);
}

void PPTokenCollector::emit_whitespace_sequence()
{
	pending_ws_ = true;
}

void PPTokenCollector::emit_new_line()
{
	Add(PPT_NEW_LINE, "\n");
}

void PPTokenCollector::emit_header_name(const string& data)
{
	Add(PPT_HEADER_NAME, data);
}

void PPTokenCollector::emit_identifier(const string& data)
{
	Add(PPT_IDENTIFIER, data);
}

void PPTokenCollector::emit_pp_number(const string& data)
{
	Add(PPT_PP_NUMBER, data);
}

void PPTokenCollector::emit_character_literal(const string& data)
{
	Add(PPT_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_character_literal(const string& data)
{
	Add(PPT_UD_CHARACTER_LITERAL, data);
}

void PPTokenCollector::emit_string_literal(const string& data)
{
	Add(PPT_STRING_LITERAL, data);
}

void PPTokenCollector::emit_user_defined_string_literal(const string& data)
{
	Add(PPT_UD_STRING_LITERAL, data);
}

void PPTokenCollector::emit_preprocessing_op_or_punc(const string& data)
{
	Add(PPT_OP_OR_PUNC, data);
}

void PPTokenCollector::emit_non_whitespace_char(const string& data)
{
	Add(PPT_NON_WHITESPACE_CHAR, data);
}

void PPTokenCollector::emit_eof()
{
	pending_ws_ = false;
}

void EmitPPToken(const PPToken& token, IPPTokenStream& output)
{
	switch (token.kind)
	{
	case PPT_NEW_LINE:
		output.emit_new_line();
		break;
	case PPT_HEADER_NAME:
		output.emit_header_name(token.data);
		break;
	case PPT_IDENTIFIER:
		output.emit_identifier(token.data);
		break;
	case PPT_PP_NUMBER:
		output.emit_pp_number(token.data);
		break;
	case PPT_CHARACTER_LITERAL:
		output.emit_character_literal(token.data);
		break;
	case PPT_UD_CHARACTER_LITERAL:
		output.emit_user_defined_character_literal(token.data);
		break;
	case PPT_STRING_LITERAL:
		output.emit_string_literal(token.data);
		break;
	case PPT_UD_STRING_LITERAL:
		output.emit_user_defined_string_literal(token.data);
		break;
	case PPT_OP_OR_PUNC:
		output.emit_preprocessing_op_or_punc(token.data);
		break;
	case PPT_NON_WHITESPACE_CHAR:
		output.emit_non_whitespace_char(token.data);
		break;
	case PPT_PLACEMARKER:
		break;
	}
}

bool IsHash(const PPToken& token)
{
	return token.kind == PPT_OP_OR_PUNC &&
		(token.data == "#" || token.data == "%:");
}

bool IsHashHash(const PPToken& token)
{
	return token.kind == PPT_OP_OR_PUNC &&
		(token.data == "##" || token.data == "%:%:");
}

bool IsOp(const PPToken& token, const char* spelling)
{
	return token.kind == PPT_OP_OR_PUNC && token.data == spelling;
}
