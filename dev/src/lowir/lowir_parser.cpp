#include "lowir/lowir_parser.h"

#include <cstdlib>
#include <stdexcept>

#include "lowir/lowir_lexer.h"

using std::runtime_error;

namespace {

ELowIRLiteralClass classify_number(const string & spelling)
{
	if(spelling.size() >= 2 && spelling[0] == '0' &&
	   (spelling[1] == 'x' || spelling[1] == 'X'))
		return LOWIR_LITERAL_INT;
	char last = spelling[spelling.size() - 1];
	if(last == 'f' || last == 'F')
		return LOWIR_LITERAL_F32;
	if(last == 'L' || last == 'l')
		return LOWIR_LITERAL_F80;
	for(size_t i = 0; i < spelling.size(); ++i)
		if(spelling[i] == '.' || spelling[i] == 'e' || spelling[i] == 'E')
			return LOWIR_LITERAL_F64;
	return LOWIR_LITERAL_INT;
}

struct Parser
{
	const vector<LowIRToken> & tokens;
	size_t pos = 0;

	explicit Parser(const vector<LowIRToken> & t) : tokens(t) {}

	const LowIRToken & peek(size_t ahead = 0) const
	{
		size_t index = pos + ahead;
		if(index >= tokens.size())
			index = tokens.size() - 1;
		return tokens[index];
	}

	const LowIRToken & advance()
	{
		const LowIRToken & token = peek();
		if(pos + 1 < tokens.size())
			++pos;
		return token;
	}

	bool at(ELowIRTokenKind kind) const { return peek().kind == kind; }

	bool at_punct(const char * spelling) const
	{
		return peek().kind == LOWIR_TOK_PUNCT && peek().text == spelling;
	}

	bool at_word(const char * spelling) const
	{
		return peek().kind == LOWIR_TOK_IDENTIFIER &&
		       peek().text == spelling;
	}

	bool accept_punct(const char * spelling)
	{
		if(!at_punct(spelling))
			return false;
		++pos;
		return true;
	}

	bool accept_word(const char * spelling)
	{
		if(!at_word(spelling))
			return false;
		++pos;
		return true;
	}

	void fail(const string & what) const
	{
		throw runtime_error("LowIR parse error near line " +
		                    std::to_string(peek().line) + ": " + what);
	}

	void expect_punct(const char * spelling)
	{
		if(!accept_punct(spelling))
			fail(string("expected '") + spelling + "'");
	}

	string expect(ELowIRTokenKind kind, const char * what)
	{
		if(!at(kind))
			fail(string("expected ") + what);
		return advance().text;
	}

	LowIRType parse_type()
	{
		if(at_word("void"))
		{
			++pos;
			LowIRType type;
			return type;
		}
		string spelling = expect(LOWIR_TOK_IDENTIFIER, "type");
		return ParseLowIRType(spelling);
	}

	LowIROperand parse_literal_operand()
	{
		LowIROperand operand;
		operand.kind = LOWIR_OPERAND_LITERAL;
		if(accept_punct("-"))
			operand.negated = true;
		if(at_word("nullptr"))
		{
			if(operand.negated)
				fail("negated nullptr literal");
			++pos;
			operand.literal = "nullptr";
			operand.literal_class = LOWIR_LITERAL_NULLPTR;
			return operand;
		}
		operand.literal = expect(LOWIR_TOK_NUMBER, "literal");
		operand.literal_class = classify_number(operand.literal);
		return operand;
	}

	bool at_value_start() const
	{
		return at(LOWIR_TOK_TEMP_NAME) || at(LOWIR_TOK_SLOT_NAME) ||
		       at(LOWIR_TOK_GLOBAL_NAME) || at(LOWIR_TOK_NUMBER) ||
		       at_punct("-") || at_word("nullptr");
	}

	LowIROperand parse_value()
	{
		LowIROperand operand;
		if(at(LOWIR_TOK_TEMP_NAME))
		{
			operand.kind = LOWIR_OPERAND_TEMP;
			operand.name = advance().text;
		}
		else if(at(LOWIR_TOK_SLOT_NAME))
		{
			operand.kind = LOWIR_OPERAND_SLOT;
			operand.name = advance().text;
		}
		else if(at(LOWIR_TOK_GLOBAL_NAME))
		{
			operand.kind = LOWIR_OPERAND_GLOBAL;
			operand.name = advance().text;
		}
		else if(at(LOWIR_TOK_NUMBER) || at_punct("-") || at_word("nullptr"))
			return parse_literal_operand();
		else
			fail("expected value operand");
		return operand;
	}

	long parse_integer_literal()
	{
		bool negated = accept_punct("-");
		string digits = expect(LOWIR_TOK_NUMBER, "integer literal");
		char * end = nullptr;
		long value = strtol(digits.c_str(), &end, 0);
		if(*end != '\0')
			fail("malformed integer literal: " + digits);
		return negated ? -value : value;
	}

	// `<bytes>x<align>` span operand on copyobj/zeroinit.
	void parse_span(LowIRInstruction & ins)
	{
		string span = expect(LOWIR_TOK_NUMBER, "byte/alignment span");
		size_t split = span.find('x');
		if(split == string::npos || split == 0 || split + 1 == span.size())
			fail("expected <bytes>x<align> span: " + span);
		char * end = nullptr;
		const string bytes_text = span.substr(0, split);
		const string align_text = span.substr(split + 1);
		ins.span_bytes = strtol(bytes_text.c_str(), &end, 10);
		if(*end != '\0')
			fail("malformed span bytes: " + span);
		ins.span_align = strtol(align_text.c_str(), &end, 10);
		if(*end != '\0')
			fail("malformed span alignment: " + span);
	}

	// Metadata values are identifiers or @symbol references; symbol
	// references keep their sigil so the validator can resolve them.
	LowIRMetadata parse_metadata()
	{
		LowIRMetadata metadata;
		expect_punct("[");
		for(;;)
		{
			string key = expect(LOWIR_TOK_IDENTIFIER, "metadata key");
			expect_punct("=");
			string value;
			if(at(LOWIR_TOK_GLOBAL_NAME))
				value = "@" + advance().text;
			else if(at(LOWIR_TOK_NUMBER))
				value = advance().text;
			else
				value = expect(LOWIR_TOK_IDENTIFIER, "metadata value");
			metadata.items.push_back(std::make_pair(key, value));
			if(!accept_punct(","))
				break;
		}
		expect_punct("]");
		return metadata;
	}

	void skip_optional_debug_location()
	{
		if(at(LOWIR_TOK_DEBUG_LOC))
			++pos;
	}

	vector<LowIRParam> parse_parameter_list()
	{
		vector<LowIRParam> params;
		expect_punct("(");
		if(!accept_punct(")"))
		{
			for(;;)
			{
				LowIRParam param;
				param.name = expect(LOWIR_TOK_TEMP_NAME, "parameter name");
				expect_punct(":");
				param.type = parse_type();
				if(at_punct("["))
					param.metadata = parse_metadata();
				params.push_back(param);
				if(!accept_punct(","))
					break;
			}
			expect_punct(")");
		}
		return params;
	}

	LowIRCallSignature parse_call_signature()
	{
		LowIRCallSignature signature;
		signature.present = true;
		signature.params = parse_parameter_list();
		expect_punct("->");
		signature.return_type = parse_type();
		if(at_punct("["))
			signature.metadata = parse_metadata();
		return signature;
	}

	void parse_call_arguments(LowIRInstruction & ins)
	{
		expect_punct("(");
		if(accept_punct(")"))
			return;
		for(;;)
		{
			ins.operands.push_back(parse_value());
			if(!accept_punct(","))
				break;
		}
		expect_punct(")");
	}

	void parse_call(LowIRInstruction & ins, bool statement_call)
	{
		ins.opcode = LOWIR_INS_CALL;
		ins.type = parse_type();
		if(statement_call && !ins.type.is_void())
			fail("statement-level call must be void");
		if(!statement_call && ins.type.is_void())
			fail("assigned call result cannot be void");
		if(at(LOWIR_TOK_TEMP_NAME))
		{
			ins.callee_is_temp = true;
			ins.callee = advance().text;
		}
		else
			ins.callee = expect(LOWIR_TOK_GLOBAL_NAME, "callee");
		parse_call_arguments(ins);
		if(accept_word("as"))
			ins.signature = parse_call_signature();
	}

	LowIRInstruction parse_rvalue(const string & result);
	LowIRInstruction parse_statement_instruction();
	void parse_function_items(LowIRFunction & function);
	LowIRFunction parse_function(bool is_definition);
	LowIRGlobal parse_global_declaration();
	LowIRGlobal parse_global_definition();
	LowIRDataItem parse_data_item();
	LowIRAlias parse_alias();
	LowIRProgram parse_program();
};

LowIRInstruction Parser::parse_rvalue(const string & result)
{
	LowIRInstruction ins;
	ins.result = result;
	if(accept_word("const"))
	{
		ins.opcode = LOWIR_INS_CONST;
		ins.type = parse_type();
		ins.operands.push_back(parse_literal_operand());
	}
	else if(accept_word("copy"))
	{
		ins.opcode = LOWIR_INS_COPY;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("addr"))
	{
		ins.opcode = LOWIR_INS_ADDR;
		LowIROperand target = parse_value();
		if(target.kind != LOWIR_OPERAND_SLOT &&
		   target.kind != LOWIR_OPERAND_GLOBAL)
			fail("addr requires a slot, global, or function");
		ins.operands.push_back(target);
	}
	else if(accept_word("load"))
	{
		ins.opcode = LOWIR_INS_LOAD;
		ins.type = parse_type();
		LowIROperand storage = parse_value();
		if(storage.is_literal())
			fail("load requires temporary, slot, or global storage");
		ins.operands.push_back(storage);
	}
	else if(accept_word("atomic_load"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_LOAD;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		parse_integer_literal();
	}
	else if(accept_word("atomic_exchange"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_EXCHANGE;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
		expect_punct(",");
		parse_integer_literal();
	}
	else if(accept_word("atomic_compare_exchange"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_COMPARE_EXCHANGE;
		ins.type = parse_type();
		for(int i = 0; i < 3; ++i)
		{
			if(i)
				expect_punct(",");
			ins.operands.push_back(parse_value());
		}
		expect_punct(",");
		parse_integer_literal();
		expect_punct(",");
		parse_integer_literal();
	}
	else if(accept_word("atomic_add_fetch"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_ADD_FETCH;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
		expect_punct(",");
		parse_integer_literal();
	}
	else if(accept_word("index"))
	{
		ins.opcode = LOWIR_INS_INDEX;
		ins.type = parse_type();
		if(at_punct("["))
			ins.metadata = parse_metadata();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("unary"))
	{
		ins.opcode = LOWIR_INS_UNARY;
		ins.operation = expect(LOWIR_TOK_IDENTIFIER, "unary operator");
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("binary"))
	{
		ins.opcode = LOWIR_INS_BINARY;
		ins.operation = expect(LOWIR_TOK_IDENTIFIER, "binary operator");
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("cmp"))
	{
		ins.opcode = LOWIR_INS_CMP;
		ins.operation = expect(LOWIR_TOK_IDENTIFIER, "compare predicate");
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("convert"))
	{
		ins.opcode = LOWIR_INS_CONVERT;
		ins.operation = expect(LOWIR_TOK_IDENTIFIER, "conversion operator");
		ins.type = parse_type();
		ins.type2 = parse_type();
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("call"))
		parse_call(ins, false);
	else if(accept_word("exception") || accept_word("exception_selector"))
	{
		ins.opcode = LOWIR_INS_EXCEPTION;
		ins.type = parse_type();
	}
	else
		fail("unknown assigned instruction");
	return ins;
}

LowIRInstruction Parser::parse_statement_instruction()
{
	LowIRInstruction ins;
	if(at(LOWIR_TOK_TEMP_NAME))
	{
		string result = advance().text;
		expect_punct("=");
		ins = parse_rvalue(result);
	}
	else if(accept_word("store"))
	{
		ins.opcode = LOWIR_INS_STORE;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		LowIROperand storage = parse_value();
		if(storage.is_literal())
			fail("store requires temporary, slot, or global storage");
		ins.operands.push_back(storage);
	}
	else if(accept_word("atomic_store"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_STORE;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
		expect_punct(",");
		parse_integer_literal();
	}
	else if(accept_word("atomic_thread_fence"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_THREAD_FENCE;
		parse_integer_literal();
	}
	else if(accept_word("atomic_signal_fence"))
	{
		ins.opcode = LOWIR_INS_ATOMIC_SIGNAL_FENCE;
		parse_integer_literal();
	}
	else if(accept_word("call"))
		parse_call(ins, true);
	else if(accept_word("copyobj"))
	{
		ins.opcode = LOWIR_INS_COPYOBJ;
		parse_span(ins);
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("zeroinit"))
	{
		ins.opcode = LOWIR_INS_ZEROINIT;
		parse_span(ins);
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("eh_try"))
	{
		ins.opcode = LOWIR_INS_EH_TRY;
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
	}
	else if(accept_word("eh_cleanup"))
	{
		ins.opcode = LOWIR_INS_EH_CLEANUP;
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
	}
	else if(accept_word("eh_catch"))
	{
		// Handler-classification markers carry no PA13 code of their own.
		ins.opcode = LOWIR_INS_EH_MARKER;
		ins.operation = "eh_catch";
		ins.eh_types.push_back(expect(LOWIR_TOK_GLOBAL_NAME, "type symbol"));
	}
	else if(accept_word("eh_filter"))
	{
		ins.opcode = LOWIR_INS_EH_MARKER;
		ins.operation = "eh_filter";
		while(at(LOWIR_TOK_GLOBAL_NAME))
		{
			ins.eh_types.push_back(advance().text);
			if(!accept_punct(","))
				break;
		}
	}
	else if(accept_word("eh_catch_all"))
	{
		ins.opcode = LOWIR_INS_EH_MARKER;
		ins.operation = "eh_catch_all";
	}
	else if(accept_word("eh_end"))
		ins.opcode = LOWIR_INS_EH_END;
	else if(accept_word("throw"))
	{
		ins.opcode = LOWIR_INS_THROW;
		ins.type = parse_type();
		ins.operands.push_back(parse_value());
	}
	else if(accept_word("resume"))
		ins.opcode = LOWIR_INS_RESUME;
	else if(accept_word("jump"))
	{
		ins.opcode = LOWIR_INS_JUMP;
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
	}
	else if(accept_word("branch"))
	{
		ins.opcode = LOWIR_INS_BRANCH;
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
		expect_punct(",");
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
	}
	else if(accept_word("switch"))
	{
		ins.opcode = LOWIR_INS_SWITCH;
		ins.operands.push_back(parse_value());
		expect_punct(",");
		ins.block_targets.push_back(expect(LOWIR_TOK_BLOCK_NAME, "block"));
		while(accept_punct(","))
		{
			ins.switch_values.push_back(parse_value());
			expect_punct(":");
			ins.block_targets.push_back(
				expect(LOWIR_TOK_BLOCK_NAME, "block"));
		}
	}
	else if(at_word("return"))
	{
		long return_line = peek().line;
		++pos;
		ins.opcode = LOWIR_INS_RETURN;
		ins.type = parse_type();
		if(peek().line == return_line && at_value_start())
			ins.operands.push_back(parse_value());
	}
	else
		fail("unknown instruction");
	skip_optional_debug_location();
	return ins;
}

void Parser::parse_function_items(LowIRFunction & function)
{
	expect_punct("{");
	while(!accept_punct("}"))
	{
		if(accept_word("slot"))
		{
			LowIRSlot slot;
			slot.name = expect(LOWIR_TOK_SLOT_NAME, "slot name");
			expect_punct(":");
			slot.type = parse_type();
			function.slots.push_back(slot);
		}
		else if(accept_word("block"))
		{
			LowIRBlock block;
			block.label = expect(LOWIR_TOK_BLOCK_NAME, "block label");
			expect_punct(":");
			function.blocks.push_back(block);
		}
		else if(at(LOWIR_TOK_EOF))
			fail("unterminated function body");
		else
		{
			if(function.blocks.empty())
				fail("instruction outside any block");
			function.blocks.back().instructions.push_back(
				parse_statement_instruction());
		}
	}
}

LowIRFunction Parser::parse_function(bool is_definition)
{
	LowIRFunction function;
	function.is_definition = is_definition;
	function.name = expect(LOWIR_TOK_GLOBAL_NAME, "function name");
	function.params = parse_parameter_list();
	expect_punct("->");
	function.return_type = parse_type();
	if(at_punct("["))
		function.metadata = parse_metadata();
	skip_optional_debug_location();
	if(is_definition)
		parse_function_items(function);
	return function;
}

LowIRGlobal Parser::parse_global_declaration()
{
	LowIRGlobal global;
	global.name = expect(LOWIR_TOK_GLOBAL_NAME, "global name");
	if(accept_punct(":"))
	{
		global.has_type = true;
		global.type = parse_type();
	}
	if(at_punct("["))
		global.metadata = parse_metadata();
	return global;
}

LowIRDataItem Parser::parse_data_item()
{
	LowIRDataItem item;
	if(accept_word("zero"))
	{
		item.kind = LOWIR_DATA_ZERO;
		item.zero_bytes = parse_integer_literal();
		if(item.zero_bytes <= 0)
			fail("non-positive zero data span");
		return item;
	}
	item.type = parse_type();
	if(item.type.kind == LOWIR_TYPE_PTR && accept_word("addr"))
	{
		item.kind = LOWIR_DATA_ADDR;
		item.symbol = expect(LOWIR_TOK_GLOBAL_NAME, "address symbol");
		if(at_punct("+") || at_punct("-"))
		{
			bool negate = advance().text == "-";
			item.has_addend = true;
			item.addend = strtol(
				expect(LOWIR_TOK_NUMBER, "addend").c_str(), nullptr, 0);
			if(negate)
				item.addend = -item.addend;
		}
		return item;
	}
	item.kind = LOWIR_DATA_SCALAR;
	item.value = parse_literal_operand();
	return item;
}

LowIRGlobal Parser::parse_global_definition()
{
	LowIRGlobal global;
	global.is_definition = true;
	global.name = expect(LOWIR_TOK_GLOBAL_NAME, "global name");
	// Bare storage keywords normalize onto [storage=...] metadata.
	string bare_storage;
	if(accept_word("readonly"))
		bare_storage = "readonly";
	else if(accept_word("thread_local"))
		bare_storage = "thread_local";
	if(accept_punct(":"))
	{
		global.has_type = true;
		global.type = parse_type();
	}
	if(at_punct("["))
		global.metadata = parse_metadata();
	if(!bare_storage.empty())
		global.metadata.items.push_back(
			std::make_pair(string("storage"), bare_storage));
	expect_punct("=");
	if(accept_punct("{"))
	{
		if(global.has_type)
			fail("structured global cannot also have a scalar type");
		global.init = LOWIR_GLOBAL_STRUCTURED;
		while(!accept_punct("}"))
		{
			if(at(LOWIR_TOK_EOF))
				fail("unterminated structured global");
			global.items.push_back(parse_data_item());
		}
		if(global.items.empty())
			fail("structured global requires at least one item");
		return global;
	}
	if(!global.has_type)
		fail("scalar global requires a type");
	if(accept_word("zero"))
		global.init = LOWIR_GLOBAL_ZERO;
	else if(accept_word("addr"))
	{
		global.init = LOWIR_GLOBAL_ADDR;
		global.addr_symbol = expect(LOWIR_TOK_GLOBAL_NAME, "address symbol");
		if(at_punct("+") || at_punct("-"))
		{
			bool negate = advance().text == "-";
			global.has_addr_addend = true;
			global.addr_addend = strtol(
				expect(LOWIR_TOK_NUMBER, "addend").c_str(), nullptr, 0);
			if(negate)
				global.addr_addend = -global.addr_addend;
		}
	}
	else
	{
		global.init = LOWIR_GLOBAL_SCALAR;
		global.scalar = parse_literal_operand();
	}
	return global;
}

LowIRAlias Parser::parse_alias()
{
	LowIRAlias alias;
	if(!accept_word("object"))
		fail("expected 'object' after 'alias'");
	alias.object_symbol = expect(LOWIR_TOK_IDENTIFIER, "alias symbol");
	expect_punct("=");
	alias.target = expect(LOWIR_TOK_GLOBAL_NAME, "alias target");
	return alias;
}

LowIRProgram Parser::parse_program()
{
	LowIRProgram program;
	while(!at(LOWIR_TOK_EOF))
	{
		if(accept_word("declare"))
		{
			if(accept_word("global"))
				program.globals.push_back(parse_global_declaration());
			else if(accept_word("function"))
				program.functions.push_back(parse_function(false));
			else
				fail("expected 'global' or 'function' after 'declare'");
		}
		else if(accept_word("global"))
			program.globals.push_back(parse_global_definition());
		else if(accept_word("function"))
			program.functions.push_back(parse_function(true));
		else if(accept_word("alias"))
			program.aliases.push_back(parse_alias());
		else
			fail("expected top-level declaration or definition");
	}
	return program;
}

}  // namespace

LowIRProgram ParseLowIRProgram(const string & text)
{
	vector<LowIRToken> tokens = LexLowIR(text);
	Parser parser(tokens);
	return parser.parse_program();
}
