#pragma once

#include <string>
#include <vector>

using std::string;

#include "IPPTokenStream.h"
#include "post_token.h"

// Tokenization part of translation phase 7: converts the phase-3
// preprocessing-token stream into typed tokens. Whitespace and new-lines
// are dropped (phase 4 is a no-op for this input class). Adjacent
// string-literal preprocessing-tokens are buffered into a maximal
// sequence and concatenated (phase 6) when any other token or eof
// arrives -- except a character literal whose body fails to decode,
// which is reported inline without breaking the sequence (reference
// behavior). Conversion failures emit invalid tokens; the stream keeps
// going.
class PostTokenizer : public IPPTokenStream
{
public:
	explicit PostTokenizer(IPostTokenStream& output);

	void emit_whitespace_sequence();
	void emit_new_line();
	void emit_header_name(const string& data);
	void emit_identifier(const string& data);
	void emit_pp_number(const string& data);
	void emit_character_literal(const string& data);
	void emit_user_defined_character_literal(const string& data);
	void emit_string_literal(const string& data);
	void emit_user_defined_string_literal(const string& data);
	void emit_preprocessing_op_or_punc(const string& data);
	void emit_non_whitespace_char(const string& data);
	void emit_eof();

private:
	void EmitCharLiteral(const string& data);
	void FlushStringSequence();

	IPostTokenStream& output_;
	std::vector<string> pending_strings_;
};
