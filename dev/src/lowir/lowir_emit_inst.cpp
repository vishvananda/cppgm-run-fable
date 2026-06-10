#include "lowir/lowir_to_cy86.h"

#include <stdexcept>

// Per-instruction CY86 templates. Each LowIR instruction expands to a
// fixed sequence so translation stays monotonic; new instructions add
// cases without disturbing existing output.

namespace {

string bits_text(long bits)
{
	return std::to_string(bits);
}

string integer_binary_opcode(const string & operation)
{
	if(operation == "add") return "iadd";
	if(operation == "sub") return "isub";
	if(operation == "mul") return "smul";
	if(operation == "div") return "sdiv";
	if(operation == "mod") return "smod";
	if(operation == "udiv") return "udiv";
	if(operation == "umod") return "umod";
	if(operation == "and") return "and";
	if(operation == "or") return "or";
	if(operation == "xor") return "xor";
	throw std::runtime_error("unexpected binary operator: " + operation);
}

string float_binary_opcode(const string & operation)
{
	if(operation == "add") return "fadd";
	if(operation == "sub") return "fsub";
	if(operation == "mul") return "fmul";
	if(operation == "div") return "fdiv";
	throw std::runtime_error("binary operator not valid on floats: " +
	                         operation);
}

string compare_opcode(const string & predicate, bool floating)
{
	if(floating)
	{
		// Unsigned predicate spellings compare the same way on floats.
		string base = predicate[0] == 'u' ? predicate.substr(1) : predicate;
		return "f" + base;
	}
	if(predicate == "eq") return "ieq";
	if(predicate == "ne") return "ine";
	if(predicate == "lt") return "slt";
	if(predicate == "le") return "sle";
	if(predicate == "gt") return "sgt";
	if(predicate == "ge") return "sge";
	return predicate;  // ult / ule / ugt / uge match CY86 spellings
}

}  // namespace

void LowIRCY86Emitter::emit_const_copy(const LowIRInstruction & in)
{
	long home = frame.home(in.result).offset;
	if(in.type.is_f80())
	{
		f80_value_to_scratch(in.operands[0], frame.scratch(1));
		f80_scratch_to_home(frame.scratch(1), home);
		return;
	}
	load_scalar('x', in.operands[0], in.type);
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_addr(const LowIRInstruction & in)
{
	address_to_x64(in.operands[0]);
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	store_result_home(in.result, pointer);
}

void LowIRCY86Emitter::emit_load(const LowIRInstruction & in)
{
	const LowIROperand & storage = in.operands[0];
	long home = frame.home(in.result).offset;
	if(in.type.is_f80())
	{
		f80_value_to_scratch(storage, frame.scratch(1));
		f80_scratch_to_home(frame.scratch(1), home);
		return;
	}
	if(storage.kind == LOWIR_OPERAND_TEMP &&
	   !operand_is_object_value(storage))
	{
		// Load through a pointer-valued temporary.
		ins("move64 x64 " + home_ref(frame.home(storage.name).offset));
		long bits = in.type.operand_bits();
		if(bits == 64)
			ins("move64 x64 [x64]");
		else if(bits == 32)
			ins("move32 x32 [x64]");
		else
		{
			ins("move64 y64 x64");
			ins("move64 x64 0");
			ins("move" + bits_text(bits) + " " + reg('x', bits) + " [y64]");
		}
	}
	else if(storage.kind == LOWIR_OPERAND_SLOT)
		load_from_location('x', home_ref(frame.home(storage.name).offset),
		                   in.type);
	else
		load_from_location('x', "[" + global_label(storage.name) + "]",
		                   in.type);
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_store(const LowIRInstruction & in)
{
	const LowIROperand & value = in.operands[0];
	const LowIROperand & storage = in.operands[1];
	if(in.type.is_f80())
	{
		f80_value_to_scratch(value, frame.scratch(1));
		if(storage.kind == LOWIR_OPERAND_TEMP &&
		   !operand_is_object_value(storage))
			ins("move64 x64 " + home_ref(frame.home(storage.name).offset));
		else
			address_to_x64(storage);
		f80_scratch_through_pointer(frame.scratch(1));
		return;
	}
	load_scalar('x', value, in.type);
	if(storage.kind == LOWIR_OPERAND_TEMP &&
	   !operand_is_object_value(storage))
	{
		ins("move64 y64 " + home_ref(frame.home(storage.name).offset));
		long bits = in.type.operand_bits();
		ins("move" + bits_text(bits) + " [y64] " + reg('x', bits));
	}
	else if(storage.kind == LOWIR_OPERAND_SLOT)
		store_to_location(home_ref(frame.home(storage.name).offset),
		                  in.type);
	else
		store_to_location("[" + global_label(storage.name) + "]", in.type);
}

void LowIRCY86Emitter::emit_atomic_load(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	load_scalar('y', in.operands[0], pointer);
	load_from_location('x', "[y64]", in.type);
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_atomic_store(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	load_scalar('y', in.operands[1], pointer);
	load_scalar('x', in.operands[0], in.type);
	long bits = in.type.operand_bits();
	ins("move" + bits_text(bits) + " [y64] " + reg('x', bits));
}

void LowIRCY86Emitter::emit_atomic_exchange(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	long bits = in.type.operand_bits();
	load_scalar('y', in.operands[0], pointer);
	load_scalar('x', in.operands[1], in.type);
	ins("move" + bits_text(bits) + " " + reg('t', bits) + " [y64]");
	ins("move" + bits_text(bits) + " [y64] " + reg('x', bits));
	ins("move64 x64 0");
	ins("move" + bits_text(bits) + " " + reg('x', bits) + " " +
	    reg('t', bits));
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_atomic_compare_exchange(
	const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_I64;
	long bits = in.type.operand_bits();
	string success = fresh_label("atomic_cmpxchg_success");
	string end = fresh_label("atomic_cmpxchg_end");
	load_scalar('y', in.operands[0], pointer);
	load_scalar('z', in.operands[1], pointer);
	ins("move" + bits_text(bits) + " " + reg('t', bits) + " [y64]");
	ins("move" + bits_text(bits) + " " + reg('x', bits) + " [z64]");
	ins("ieq" + bits_text(bits) + " x8 " + reg('t', bits) + " " +
	    reg('x', bits));
	ins("jumpif x8 " + success);
	ins("move" + bits_text(bits) + " [z64] " + reg('t', bits));
	ins("move64 x64 0");
	store_result_home(in.result, result_type);
	ins("jump " + end);
	label(success);
	load_scalar('x', in.operands[2], in.type);
	ins("move" + bits_text(bits) + " [y64] " + reg('x', bits));
	ins("move64 x64 1");
	store_result_home(in.result, result_type);
	label(end);
}

void LowIRCY86Emitter::emit_atomic_add_fetch(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	long bits = in.type.operand_bits();
	load_scalar('y', in.operands[0], pointer);
	ins("move" + bits_text(bits) + " " + reg('x', bits) + " [y64]");
	load_scalar('z', in.operands[1], in.type);
	ins("iadd" + bits_text(bits) + " " + reg('x', bits) + " " +
	    reg('x', bits) + " " + reg('z', bits));
	ins("move" + bits_text(bits) + " [y64] " + reg('x', bits));
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_index(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	LowIRType offset_type;
	offset_type.kind = LOWIR_TYPE_I64;
	load_scalar('y', in.operands[0], pointer);
	load_scalar('x', in.operands[1], offset_type);
	long element = in.type.element_bytes();
	if(element != 1)
	{
		ins("move64 z64 " + std::to_string(element));
		ins("smul64 x64 x64 z64");
	}
	ins("iadd64 x64 y64 x64");
	store_result_home(in.result, pointer);
}

void LowIRCY86Emitter::emit_unary(const LowIRInstruction & in)
{
	long bits = in.type.operand_bits();
	if(in.operation == "neg" && in.type.is_f80())
	{
		LowIROperand zero;
		zero.kind = LOWIR_OPERAND_LITERAL;
		zero.literal = "0.0L";
		zero.literal_class = LOWIR_LITERAL_F80;
		f80_value_to_scratch(in.operands[0], frame.scratch(1));
		f80_value_to_scratch(zero, frame.scratch(2));
		ins("fsub80 " + home_ref(frame.scratch(3)) + " " +
		    home_ref(frame.scratch(2)) + " " + home_ref(frame.scratch(1)));
		f80_pad(frame.scratch(3));
		f80_scratch_to_home(frame.scratch(3),
		                    frame.home(in.result).offset);
		return;
	}
	load_scalar('x', in.operands[0], in.type);
	if(in.operation == "neg" && in.type.is_float())
	{
		string zero = in.type.kind == LOWIR_TYPE_F32 ? "0.0f" : "0.0";
		ins("move" + bits_text(bits) + " " + reg('y', bits) + " " + zero);
		ins("fsub" + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('y', bits) + " " + reg('x', bits));
	}
	else if(in.operation == "neg")
	{
		ins("move64 y64 0");
		ins("isub" + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('y', bits) + " " + reg('x', bits));
	}
	else if(in.operation == "not")
	{
		ins("ieq" + bits_text(bits) + " z8 " + reg('x', bits) + " 0");
		ins("move64 x64 0");
		ins("move8 x8 z8");
	}
	else if(in.operation == "bitnot")
		ins("not" + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('x', bits));
	else if(in.operation == "bswap")
		ins("bswap" + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('x', bits));
	// decay is a semantic identity on pointers: load, then store.
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_binary(const LowIRInstruction & in)
{
	long bits = in.type.operand_bits();
	if(in.type.is_f80())
	{
		f80_value_to_scratch(in.operands[0], frame.scratch(1));
		f80_value_to_scratch(in.operands[1], frame.scratch(2));
		ins(float_binary_opcode(in.operation) + "80 " +
		    home_ref(frame.scratch(3)) + " " +
		    home_ref(frame.scratch(1)) + " " + home_ref(frame.scratch(2)));
		f80_pad(frame.scratch(3));
		f80_scratch_to_home(frame.scratch(3),
		                    frame.home(in.result).offset);
		return;
	}
	load_scalar('y', in.operands[0], in.type);
	load_scalar('x', in.operands[1], in.type);
	if(in.operation == "shl" || in.operation == "shr" ||
	   in.operation == "ushr")
	{
		string opcode = in.operation == "shl" ? "lshift"
		              : in.operation == "shr" ? "srshift" : "urshift";
		ins("move64 z64 x64");
		ins("move8 x8 z8");
		ins(opcode + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('y', bits) + " x8");
	}
	else
	{
		string opcode = in.type.is_float()
			? float_binary_opcode(in.operation)
			: integer_binary_opcode(in.operation);
		ins(opcode + bits_text(bits) + " " + reg('x', bits) + " " +
		    reg('y', bits) + " " + reg('x', bits));
	}
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_cmp(const LowIRInstruction & in)
{
	LowIRType result_type;
	result_type.kind = LOWIR_TYPE_I64;
	if(in.type.is_f80())
	{
		f80_value_to_scratch(in.operands[0], frame.scratch(1));
		f80_value_to_scratch(in.operands[1], frame.scratch(2));
		ins(compare_opcode(in.operation, true) + "80 z8 " +
		    home_ref(frame.scratch(1)) + " " + home_ref(frame.scratch(2)));
	}
	else
	{
		long bits = in.type.operand_bits();
		load_scalar('y', in.operands[0], in.type);
		load_scalar('x', in.operands[1], in.type);
		ins(compare_opcode(in.operation, in.type.is_float()) +
		    bits_text(bits) + " z8 " + reg('y', bits) + " " +
		    reg('x', bits));
	}
	ins("move64 x64 0");
	ins("move8 x8 z8");
	store_result_home(in.result, result_type);
}

void LowIRCY86Emitter::emit_convert(const LowIRInstruction & in)
{
	const LowIRType & dst = in.type;
	const LowIRType & src = in.type2;
	const LowIROperand & value = in.operands[0];
	long home = frame.home(in.result).offset;
	long scratch = frame.scratch(1);
	if(in.operation == "zext" || in.operation == "trunc")
	{
		// The width-typed source load already zero-extends.
		load_scalar('x', value, src);
		store_result_home(in.result, dst);
	}
	else if(in.operation == "sext")
	{
		load_scalar('x', value, src);
		ins("move8 t8 " + std::to_string(64 - src.operand_bits()));
		ins("lshift64 x64 x64 t8");
		ins("srshift64 x64 x64 t8");
		store_result_home(in.result, dst);
	}
	else if(in.operation == "sitofp" || in.operation == "uitofp")
	{
		load_scalar('x', value, src);
		string sign = in.operation == "sitofp" ? "s" : "u";
		long bits = src.operand_bits();
		ins(sign + bits_text(bits) + "convf80 " + home_ref(scratch) + " " +
		    reg('x', bits));
		f80_pad(scratch);
		if(dst.is_f80())
			f80_scratch_to_home(scratch, home);
		else
			ins("f80convf" + bits_text(dst.operand_bits()) + " " +
			    home_ref(home) + " " + home_ref(scratch));
	}
	else if(in.operation == "fptosi" || in.operation == "fptoui")
	{
		convert_source_to_scratch(value, src, scratch);
		string sign = in.operation == "fptosi" ? "s" : "u";
		ins("f80conv" + sign + bits_text(dst.operand_bits()) + " " +
		    home_ref(home) + " " + home_ref(scratch));
	}
	else  // fpext / fptrunc
	{
		convert_source_to_scratch(value, src, scratch);
		if(dst.is_f80())
			f80_scratch_to_home(scratch, home);
		else
			ins("f80convf" + bits_text(dst.operand_bits()) + " " +
			    home_ref(home) + " " + home_ref(scratch));
	}
}

// Calls follow the PA13 boundary: the first four argument units ride in
// x64/y64/z64/t64, later units ride on the caller stack, an f80/obj
// result adds a hidden leading result pointer, and f80/obj values pass
// by address. Indirect callees are pushed below the stack arguments so
// `call [sp]` can reach them.
void LowIRCY86Emitter::emit_call(const LowIRInstruction & in)
{
	static const char arg_registers[4] = {'x', 'y', 'z', 't'};
	vector<LowIRType> param_types;
	bool direct = !in.callee_is_temp && info.is_function(in.callee);
	const vector<LowIRParam> & declared = direct
		? info.functions.find(in.callee)->second->params
		: in.signature.params;
	for(size_t i = 0; i < declared.size(); ++i)
		param_types.push_back(declared[i].type);

	bool hidden = in.type.is_f80() || in.type.is_obj();
	size_t units = in.operands.size() + (hidden ? 1 : 0);
	long stack_units = units > 4 ? (long)units - 4 : 0;
	long pushed = 8 * stack_units;
	if(stack_units > 0)
		ins("isub64 sp sp " + std::to_string(8 * stack_units));
	if(!direct)
	{
		LowIRType pointer;
		pointer.kind = LOWIR_TYPE_PTR;
		LowIROperand callee;
		callee.kind = in.callee_is_temp ? LOWIR_OPERAND_TEMP
		                                : LOWIR_OPERAND_GLOBAL;
		callee.name = in.callee;
		load_scalar('x', callee, pointer);
		ins("isub64 sp sp 8");
		ins("move64 [sp] x64");
		pushed += 8;
	}
	for(size_t unit = 0; unit < units; ++unit)
	{
		bool to_register = unit < 4;
		char target = to_register ? arg_registers[unit] : 'x';
		if(hidden && unit == 0)
		{
			ins("isub64 x64 bp " +
			    std::to_string(frame.home(in.result).offset));
			ins("move64 " + reg(target, 64) + " x64");
			continue;
		}
		size_t arg = unit - (hidden ? 1 : 0);
		const LowIROperand & operand = in.operands[arg];
		LowIRType type = arg < param_types.size() ? param_types[arg]
			: operand_value_type(operand);
		bool by_address = type.is_f80() || type.is_obj() ||
			operand.kind == LOWIR_OPERAND_SLOT ||
			operand_is_object_value(operand);
		if(by_address)
		{
			if(operand.is_literal())
			{
				if(operand.literal_class != LOWIR_LITERAL_F80)
					throw std::runtime_error(
						"by-address literal argument requires an f80 "
						"spelling");
				long home =
					frame.call_literal_homes.at(call_literal_index++);
				f80_value_to_scratch(operand, home);
				ins("isub64 x64 bp " + std::to_string(home));
			}
			else
				address_to_x64(operand);
			ins("move64 " + reg(target, 64) + " x64");
		}
		else
			load_scalar(target, operand, type);
		if(!to_register)
		{
			long slot = (direct ? 0 : 8) + 8 * (long)(unit - 4);
			string location = slot == 0 ? "[sp]"
				: "[sp+" + std::to_string(slot) + "]";
			LowIRType stored = type;
			if(by_address)
				stored.kind = LOWIR_TYPE_PTR;
			ins("move64 " + location + " 0");
			store_to_location(location, stored);
		}
	}
	if(direct)
		ins("call " + function_label(in.callee));
	else
		ins("call [sp]");
	if(pushed > 0)
		ins("iadd64 sp sp " + std::to_string(pushed));
	if(!hidden && !in.type.is_void())
		store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_copyobj(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	const LowIROperand & source = in.operands[0];
	const LowIROperand & destination = in.operands[1];
	load_scalar('x', destination, pointer);
	if(operand_is_object_value(source) &&
	   source.kind == LOWIR_OPERAND_TEMP)
		ins("isub64 y64 bp " +
		    std::to_string(frame.home(source.name).offset));
	else
		load_scalar('y', source, pointer);
	long copied = 0;
	while(copied < in.span_bytes)
	{
		long step = LowIRChunkBytes(in.span_bytes - copied);
		if(copied > 0)
		{
			ins("iadd64 x64 x64 " + std::to_string(step));
			ins("iadd64 y64 y64 " + std::to_string(step));
		}
		long bits = step * 8;
		ins("move" + bits_text(bits) + " " + reg('z', bits) + " [y64]");
		ins("move" + bits_text(bits) + " [x64] " + reg('z', bits));
		copied += step;
	}
}

void LowIRCY86Emitter::emit_zeroinit(const LowIRInstruction & in)
{
	LowIRType pointer;
	pointer.kind = LOWIR_TYPE_PTR;
	load_scalar('x', in.operands[0], pointer);
	ins("move64 z64 0");
	long zeroed = 0;
	while(zeroed < in.span_bytes)
	{
		long step = LowIRChunkBytes(in.span_bytes - zeroed);
		if(zeroed > 0)
			ins("iadd64 x64 x64 " + std::to_string(step));
		long bits = step * 8;
		ins("move" + bits_text(bits) + " [x64] " + reg('z', bits));
		zeroed += step;
	}
}

// eh_try / eh_cleanup: push a 32-byte handler record (previous top,
// handler address, bp, restored sp) and make it the new dynamic top.
void LowIRCY86Emitter::emit_eh_push(const LowIRInstruction & in)
{
	ins("isub64 sp sp 32");
	ins("move64 z64 [g____cppgm_eh_top]");
	ins("move64 [sp] z64");
	ins("move64 z64 " + block_label(in.block_targets[0]));
	ins("move64 [sp+8] z64");
	ins("move64 [sp+16] bp");
	ins("move64 z64 sp");
	ins("iadd64 z64 z64 32");
	ins("move64 [sp+24] z64");
	ins("move64 z64 sp");
	ins("move64 [g____cppgm_eh_top] z64");
}

void LowIRCY86Emitter::emit_eh_end()
{
	ins("move64 x64 [g____cppgm_eh_top]");
	ins("move64 y64 [x64]");
	ins("move64 [g____cppgm_eh_top] y64");
	ins("move64 sp x64");
	ins("iadd64 sp sp 32");
}

// Shared throw/resume tail: enter the innermost handler, or exit
// through the unhandled-exception runtime when none is installed.
void LowIRCY86Emitter::emit_eh_dispatch()
{
	string handler = fresh_label("eh_handler");
	string unhandled = fresh_label("eh_unhandled");
	ins("move64 x64 [g____cppgm_eh_top]");
	ins("ieq64 z8 x64 0");
	ins("jumpif z8 " + unhandled);
	label(handler);
	ins("move64 y64 [x64]");
	ins("move64 [g____cppgm_eh_top] y64");
	ins("move64 z64 [x64+8]");
	ins("move64 bp [x64+16]");
	ins("move64 sp [x64+24]");
	ins("jump z64");
	label(unhandled);
	ins("move64 x64 [g____cppgm_eh_value]");
	ins("call fn____cppgm_eh_unhandled");
	ins("syscall1 t64 60 x64");
	blank();
}

void LowIRCY86Emitter::emit_throw(const LowIRInstruction & in)
{
	load_scalar('x', in.operands[0], in.type);
	ins("move64 [g____cppgm_eh_value] x64");
	emit_eh_dispatch();
}

void LowIRCY86Emitter::emit_exception(const LowIRInstruction & in)
{
	ins("move64 x64 [g____cppgm_eh_value]");
	store_result_home(in.result, in.type);
}

void LowIRCY86Emitter::emit_branch(const LowIRInstruction & in)
{
	LowIRType condition_type;
	condition_type.kind = LOWIR_TYPE_I64;
	load_scalar('x', in.operands[0], condition_type);
	ins("ieq64 z8 x64 0");
	ins("jumpif z8 " + block_label(in.block_targets[1]));
	ins("jump " + block_label(in.block_targets[0]));
}

void LowIRCY86Emitter::emit_switch(const LowIRInstruction & in)
{
	LowIRType selector_type;
	selector_type.kind = LOWIR_TYPE_I64;
	load_scalar('x', in.operands[0], selector_type);
	for(size_t i = 0; i < in.switch_values.size(); ++i)
	{
		load_scalar('t', in.switch_values[i], selector_type);
		ins("ieq64 z8 x64 t64");
		ins("jumpif z8 " + block_label(in.block_targets[i + 1]));
	}
	ins("jump " + block_label(in.block_targets[0]));
}

void LowIRCY86Emitter::emit_return(const LowIRInstruction & in)
{
	if(in.operands.empty())
	{
		ins("jump " + block_label("epilogue"));
		return;
	}
	const LowIROperand & value = in.operands[0];
	if(in.type.is_f80())
	{
		f80_value_to_scratch(value, frame.scratch(1));
		ins("move64 x64 " + home_ref(frame.ret_pointer_offset));
		f80_scratch_through_pointer(frame.scratch(1));
	}
	else if(in.type.is_obj())
	{
		address_to_x64(value);
		ins("move64 y64 " + home_ref(frame.ret_pointer_offset));
		long copied = 0;
		while(copied < in.type.obj_bytes)
		{
			long step = LowIRChunkBytes(in.type.obj_bytes - copied);
			long bits = step * 8;
			string suffix = copied == 0 ? ""
				: "+" + std::to_string(copied);
			ins("move" + bits_text(bits) + " " + reg('z', bits) +
			    " [x64" + suffix + "]");
			ins("move" + bits_text(bits) + " [y64" + suffix + "] " +
			    reg('z', bits));
			copied += step;
		}
	}
	else
		load_scalar('x', value, in.type);
	ins("jump " + block_label("epilogue"));
}

void LowIRCY86Emitter::emit_instruction(const LowIRInstruction & in)
{
	switch(in.opcode)
	{
	case LOWIR_INS_CONST:
	case LOWIR_INS_COPY: emit_const_copy(in); break;
	case LOWIR_INS_ADDR: emit_addr(in); break;
	case LOWIR_INS_LOAD: emit_load(in); break;
	case LOWIR_INS_STORE: emit_store(in); break;
	case LOWIR_INS_ATOMIC_LOAD: emit_atomic_load(in); break;
	case LOWIR_INS_ATOMIC_STORE: emit_atomic_store(in); break;
	case LOWIR_INS_ATOMIC_EXCHANGE: emit_atomic_exchange(in); break;
	case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
		emit_atomic_compare_exchange(in);
		break;
	case LOWIR_INS_ATOMIC_ADD_FETCH: emit_atomic_add_fetch(in); break;
	case LOWIR_INS_ATOMIC_THREAD_FENCE:
	case LOWIR_INS_ATOMIC_SIGNAL_FENCE:
		break;  // fences lower to no code: CY86 has no thread model
	case LOWIR_INS_INDEX: emit_index(in); break;
	case LOWIR_INS_UNARY: emit_unary(in); break;
	case LOWIR_INS_BINARY: emit_binary(in); break;
	case LOWIR_INS_CMP: emit_cmp(in); break;
	case LOWIR_INS_CONVERT: emit_convert(in); break;
	case LOWIR_INS_CALL: emit_call(in); break;
	case LOWIR_INS_COPYOBJ: emit_copyobj(in); break;
	case LOWIR_INS_ZEROINIT: emit_zeroinit(in); break;
	case LOWIR_INS_EXCEPTION: emit_exception(in); break;
	case LOWIR_INS_EH_TRY:
	case LOWIR_INS_EH_CLEANUP: emit_eh_push(in); break;
	case LOWIR_INS_EH_END: emit_eh_end(); break;
	case LOWIR_INS_EH_MARKER:
		break;  // handler classification carries no code in PA13
	case LOWIR_INS_THROW: emit_throw(in); break;
	case LOWIR_INS_RESUME: emit_eh_dispatch(); break;
	case LOWIR_INS_JUMP:
		ins("jump " + block_label(in.block_targets[0]));
		break;
	case LOWIR_INS_BRANCH: emit_branch(in); break;
	case LOWIR_INS_SWITCH: emit_switch(in); break;
	case LOWIR_INS_RETURN: emit_return(in); break;
	}
}
