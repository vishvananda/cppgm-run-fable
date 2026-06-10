#include "post_tokenizer.h"

#include "numeric_literals.h"
#include "text_literals.h"

PostTokenizer::PostTokenizer(IPostTokenStream& output)
	: output_(output)
{}

void PostTokenizer::emit_whitespace_sequence()
{
	// Whitespace separates but does not terminate a string-literal
	// sequence (phase 6 concatenation spans whitespace).
}

void PostTokenizer::emit_new_line()
{
}

void PostTokenizer::emit_header_name(const string& data)
{
	FlushStringSequence();
	output_.emit(MakeInvalidToken(data));
}

void PostTokenizer::emit_identifier(const string& data)
{
	FlushStringSequence();
	PostToken token;
	token.source = data;
	if (LookupSimpleTokenType(data, token.token_type))
		token.kind = PTK_SIMPLE;
	else
		token.kind = PTK_IDENTIFIER;
	output_.emit(token);
}

void PostTokenizer::emit_pp_number(const string& data)
{
	FlushStringSequence();
	output_.emit(AnalyzePPNumber(data));
}

void PostTokenizer::emit_character_literal(const string& data)
{
	FlushStringSequence();
	output_.emit(AnalyzeCharLiteral(data));
}

void PostTokenizer::emit_user_defined_character_literal(const string& data)
{
	FlushStringSequence();
	output_.emit(AnalyzeCharLiteral(data));
}

void PostTokenizer::emit_string_literal(const string& data)
{
	pending_strings_.push_back(data);
}

void PostTokenizer::emit_user_defined_string_literal(const string& data)
{
	pending_strings_.push_back(data);
}

void PostTokenizer::emit_preprocessing_op_or_punc(const string& data)
{
	FlushStringSequence();
	PostToken token;
	token.source = data;
	// #, ##, %: and %:%: have no phase-7 mapping and stay invalid
	if (LookupSimpleTokenType(data, token.token_type))
		token.kind = PTK_SIMPLE;
	else
		token.kind = PTK_INVALID;
	output_.emit(token);
}

void PostTokenizer::emit_non_whitespace_char(const string& data)
{
	FlushStringSequence();
	output_.emit(MakeInvalidToken(data));
}

void PostTokenizer::emit_eof()
{
	FlushStringSequence();
	PostToken token;
	token.kind = PTK_EOF;
	output_.emit(token);
}

void PostTokenizer::FlushStringSequence()
{
	if (pending_strings_.empty())
		return;
	std::vector<string> sources;
	sources.swap(pending_strings_);
	output_.emit(AnalyzeStringSequence(sources));
}
