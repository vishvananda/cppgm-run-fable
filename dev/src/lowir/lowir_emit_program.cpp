#include "lowir/lowir_to_cy86.h"

#include <cstdlib>
#include <cstring>

// Program-level CY86 emission: the start block with init/fini hooks,
// function prologue/epilogue framing, the synthesized exception runtime,
// and global data images.

namespace {

bool uses_exception_runtime(const LowIRInstruction & ins)
{
	switch(ins.opcode)
	{
	case LOWIR_INS_EH_TRY:
	case LOWIR_INS_EH_CLEANUP:
	case LOWIR_INS_EH_END:
	case LOWIR_INS_EH_MARKER:
	case LOWIR_INS_THROW:
	case LOWIR_INS_RESUME:
	case LOWIR_INS_EXCEPTION:
		return true;
	default:
		return false;
	}
}

}  // namespace

void LowIRCY86Emitter::emit_start_block()
{
	label("start");
	ins("move64 bp sp");
	if(!info.init_function.empty())
		ins("call " + function_label(info.init_function));
	ins("call " + function_label(info.entry_function));
	if(!info.fini_function.empty())
	{
		// The entry result rides across the fini call on the stack.
		ins("isub64 sp sp 8");
		ins("move64 [sp] x64");
		ins("call " + function_label(info.fini_function));
		ins("move64 x64 [sp]");
		ins("iadd64 sp sp 8");
	}
	ins("syscall1 t64 60 x64");
}

// Captures incoming arguments into their frame homes. f80/obj parameters
// arrive as pointers to caller-owned storage and are copied in chunks.
void LowIRCY86Emitter::emit_parameter_captures()
{
	static const char arg_registers[4] = {'x', 'y', 'z', 't'};
	size_t unit = 0;
	if(frame.ret_pointer_offset != 0)
	{
		ins("move64 " + home_ref(frame.ret_pointer_offset) + " x64");
		unit = 1;
	}
	for(size_t i = 0; i < function->params.size(); ++i, ++unit)
	{
		const LowIRParam & param = function->params[i];
		const LowIRHome & home = frame.home(param.name);
		bool by_address = param.type.is_f80() || param.type.is_obj();
		if(unit < 4)
		{
			char source = arg_registers[unit];
			if(!by_address)
			{
				ins("move64 " + home_ref(home.offset) + " " +
				    reg(source, 64));
				continue;
			}
			ins("move64 x64 " + reg(source, 64));
		}
		else
		{
			ins("move64 x64 [bp+" +
			    std::to_string(16 + 8 * (long)(unit - 4)) + "]");
			if(!by_address)
			{
				ins("move64 " + home_ref(home.offset) + " x64");
				continue;
			}
		}
		long total = param.type.is_f80() ? 16 : param.type.obj_bytes;
		long copied = 0;
		while(copied < total)
		{
			long step = LowIRChunkBytes(total - copied);
			long bits = step * 8;
			string source_ref = copied == 0 ? "[x64]"
				: "[x64+" + std::to_string(copied) + "]";
			string bits_text = std::to_string(bits);
			ins("move" + bits_text + " " + reg('z', bits) + " " +
			    source_ref);
			ins("move" + bits_text + " [bp-" +
			    std::to_string(home.offset - copied) + "] " +
			    reg('z', bits));
			copied += step;
		}
	}
}

void LowIRCY86Emitter::emit_function(const LowIRFunction & f)
{
	function = &f;
	frame = ComputeLowIRFrame(f);
	label(function_label(f.name));
	ins("isub64 sp sp 8");
	ins("move64 [sp] bp");
	ins("move64 bp sp");
	if(frame.frame_size > 0)
		ins("isub64 sp sp " + std::to_string(frame.frame_size));
	emit_parameter_captures();
	for(size_t b = 0; b < f.blocks.size(); ++b)
	{
		const LowIRBlock & block = f.blocks[b];
		label(block_label(block.label));
		for(size_t i = 0; i < block.instructions.size(); ++i)
			emit_instruction(block.instructions[i]);
	}
	label(block_label("epilogue"));
	ins("move64 sp bp");
	ins("move64 bp [sp]");
	ins("iadd64 sp sp 8");
	ins("ret");
	function = nullptr;
}

// The unhandled-exception runtime exits with the payload as the status.
void LowIRCY86Emitter::emit_eh_runtime_function()
{
	label("fn____cppgm_eh_unhandled");
	ins("syscall1 t64 60 x64");
}

void LowIRCY86Emitter::emit_eh_runtime_globals()
{
	blank();
	label("g____cppgm_eh_top");
	ins("data64 0");
	blank();
	label("g____cppgm_eh_value");
	ins("data64 0");
}

// Encodes an f80 literal's 80-bit storage image: the low mantissa word,
// the sign/exponent word, then six zero padding bytes to fill 16 bytes.
void LowIRCY86Emitter::emit_f80_data(const LowIROperand & literal)
{
	long double value = strtold(literal.literal.c_str(), nullptr);
	if(literal.negated)
		value = -value;
	unsigned char bytes[10];
	std::memset(bytes, 0, sizeof(bytes));
	std::memcpy(bytes, &value, sizeof(bytes));
	long long low = 0;
	std::memcpy(&low, bytes, 8);
	unsigned exponent_word = bytes[8] | (unsigned(bytes[9]) << 8);
	ins("data64 " + std::to_string(low));
	ins("data16 " + std::to_string(exponent_word));
	for(int i = 0; i < 6; ++i)
		ins("data8 0");
}

// Address initializers reference the target's emitted label, optionally
// inside a parenthesized label+offset immediate.
string LowIRCY86Emitter::address_init_text(const string & symbol,
                                           bool has_addend,
                                           long addend) const
{
	string text = info.is_function(symbol) ? function_label(symbol)
	                                       : global_label(symbol);
	if(!has_addend)
		return text;
	string sign = addend < 0 ? "-" : "+";
	long magnitude = addend < 0 ? -addend : addend;
	return "(" + text + sign + std::to_string(magnitude) + ")";
}

// Structured data items lay out in order with explicit zero padding up
// to each item's natural alignment; `offset` tracks the running size.
void LowIRCY86Emitter::emit_data_item(const LowIRDataItem & item,
                                      long & offset)
{
	long align = 1;
	if(item.kind == LOWIR_DATA_ADDR)
		align = 8;
	else if(item.kind == LOWIR_DATA_SCALAR)
		align = item.type.element_bytes();
	while(align > 1 && offset % align != 0)
	{
		ins("data8 0");
		++offset;
	}
	if(item.kind == LOWIR_DATA_ZERO)
	{
		for(long i = 0; i < item.zero_bytes; ++i)
			ins("data8 0");
		offset += item.zero_bytes;
	}
	else if(item.kind == LOWIR_DATA_ADDR)
	{
		ins("data64 " +
		    address_init_text(item.symbol, item.has_addend, item.addend));
		offset += 8;
	}
	else if(item.type.is_f80())
	{
		emit_f80_data(item.value);
		offset += 16;
	}
	else
	{
		long bits = item.type.operand_bits();
		ins("data" + std::to_string(bits) + " " + literal_text(item.value));
		offset += bits / 8;
	}
}

void LowIRCY86Emitter::emit_global(const LowIRGlobal & global)
{
	label(global_label(global.name));
	if(global.init == LOWIR_GLOBAL_STRUCTURED)
	{
		long offset = 0;
		for(size_t i = 0; i < global.items.size(); ++i)
			emit_data_item(global.items[i], offset);
		return;
	}
	if(global.type.is_f80())
	{
		LowIROperand value = global.scalar;
		if(global.init != LOWIR_GLOBAL_SCALAR)
		{
			value.kind = LOWIR_OPERAND_LITERAL;
			value.literal = "0";
			value.negated = false;
			value.literal_class = LOWIR_LITERAL_F80;
		}
		emit_f80_data(value);
		return;
	}
	string text;
	if(global.init == LOWIR_GLOBAL_ZERO)
		text = "0";
	else if(global.init == LOWIR_GLOBAL_SCALAR)
		text = literal_text(global.scalar);
	else
		text = address_init_text(global.addr_symbol,
		                         global.has_addr_addend,
		                         global.addr_addend);
	ins("data" + std::to_string(global.type.operand_bits()) + " " + text);
}

string LowIRCY86Emitter::run()
{
	for(size_t f = 0; f < program.functions.size() && !uses_eh; ++f)
	{
		const LowIRFunction & fn = program.functions[f];
		for(size_t b = 0; b < fn.blocks.size() && !uses_eh; ++b)
			for(size_t i = 0; i < fn.blocks[b].instructions.size(); ++i)
				if(uses_exception_runtime(fn.blocks[b].instructions[i]))
				{
					uses_eh = true;
					break;
				}
	}
	emit_start_block();
	for(size_t i = 0; i < program.functions.size(); ++i)
		if(program.functions[i].is_definition)
		{
			blank();
			emit_function(program.functions[i]);
		}
	if(uses_eh)
	{
		blank();
		emit_eh_runtime_function();
	}
	for(size_t i = 0; i < program.globals.size(); ++i)
		if(program.globals[i].is_definition)
		{
			blank();
			emit_global(program.globals[i]);
		}
	if(uses_eh)
		emit_eh_runtime_globals();
	return out;
}

string TranslateLowIRProgramToCY86(const LowIRProgram & program,
                                   const LowIRProgramInfo & info)
{
	LowIRCY86Emitter emitter(program, info);
	return emitter.run();
}
