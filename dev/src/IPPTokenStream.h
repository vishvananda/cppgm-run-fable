#pragma once

// Optional side channel of TokenizePPTokens: receives the 1-based
// physical source line (a phase-1 byte-stream fact; line splices and
// block comments hide new-lines from the phase-3 stream) of the next
// token before its emit_* call. PA5 stamps tokens with it for
// __FILE__/__LINE__ tracking; earlier assignments pass no sink.
struct IPPTokenLineSink
{
	virtual void token_start_line(long line) = 0;

	virtual ~IPPTokenLineSink() {}
};

struct IPPTokenStream
{
	virtual void emit_whitespace_sequence() = 0;
	virtual void emit_new_line() = 0;
	virtual void emit_header_name(const string& data) = 0;
	virtual void emit_identifier(const string& data) = 0;
	virtual void emit_pp_number(const string& data) = 0;
	virtual void emit_character_literal(const string& data) = 0;
	virtual void emit_user_defined_character_literal(const string& data) = 0;
	virtual void emit_string_literal(const string& data) = 0;
	virtual void emit_user_defined_string_literal(const string& data) = 0;
	virtual void emit_preprocessing_op_or_punc(const string& data) = 0;
	virtual void emit_non_whitespace_char(const string& data) = 0;
	virtual void emit_eof() = 0;

	virtual ~IPPTokenStream() {}
};
