#include "lowir/lowir_to_cy86.h"

#include <stdexcept>

// Operand and f80 movement helpers shared by the instruction templates.
// CY86 register conventions throughout the emitter: x is the primary
// value/result register, y the left/pointer register, z the data-shuttle
// register, t the secondary scratch register.

void LowIRCY86Emitter::ins(const string & text)
{
	out += "\t";
	out += text;
	out += ";\n";
}

void LowIRCY86Emitter::label(const string & name)
{
	out += name;
	out += ":\n";
}

void LowIRCY86Emitter::blank()
{
	out += "\n";
}

string LowIRCY86Emitter::reg(char file, long bits) const
{
	return string(1, file) + std::to_string(bits);
}

string LowIRCY86Emitter::home_ref(long offset) const
{
	return "[bp-" + std::to_string(offset) + "]";
}

string LowIRCY86Emitter::home_high_ref(long offset) const
{
	return "[bp-" + std::to_string(offset - 8) + "]";
}

string LowIRCY86Emitter::global_label(const string & name) const
{
	return "g__" + name;
}

string LowIRCY86Emitter::function_label(const string & name) const
{
	return "fn__" + name;
}

string LowIRCY86Emitter::block_label(const string & block) const
{
	return function_label(function->name) + "__" + block;
}

string LowIRCY86Emitter::literal_text(const LowIROperand & operand) const
{
	if(operand.literal_class == LOWIR_LITERAL_NULLPTR)
		return "0";
	return operand.negated ? "-" + operand.literal : operand.literal;
}

string LowIRCY86Emitter::fresh_label(const string & stem)
{
	return "__" + stem + "__" + std::to_string(label_counter++);
}

const LowIRHome * LowIRCY86Emitter::find_home(const string & name) const
{
	map<string, LowIRHome>::const_iterator found = frame.values.find(name);
	return found == frame.values.end() ? nullptr : &found->second;
}

// True when the operand denotes an f80/obj value whose "register" form is
// its home address (call arguments, copyobj sources, obj returns).
bool LowIRCY86Emitter::operand_is_object_value(
	const LowIROperand & operand) const
{
	if(operand.kind != LOWIR_OPERAND_TEMP &&
	   operand.kind != LOWIR_OPERAND_SLOT)
		return false;
	const LowIRHome * home = find_home(operand.name);
	return home && (home->type.is_f80() || home->type.is_obj());
}

// The operand's own value type, used where no declared parameter type
// applies (variadic argument tails). Slots and function symbols act as
// pointers; data globals default to i64 loads.
LowIRType LowIRCY86Emitter::operand_value_type(
	const LowIROperand & operand) const
{
	LowIRType type;
	type.kind = LOWIR_TYPE_I64;
	if(operand.kind == LOWIR_OPERAND_TEMP)
	{
		const LowIRHome * home = find_home(operand.name);
		if(home)
			return home->type;
	}
	else if(operand.kind == LOWIR_OPERAND_SLOT)
		type.kind = LOWIR_TYPE_PTR;
	else if(operand.is_literal())
	{
		if(operand.literal_class == LOWIR_LITERAL_F32)
			type.kind = LOWIR_TYPE_F32;
		else if(operand.literal_class == LOWIR_LITERAL_F64)
			type.kind = LOWIR_TYPE_F64;
		else if(operand.literal_class == LOWIR_LITERAL_F80)
			type.kind = LOWIR_TYPE_F80;
		else if(operand.literal_class == LOWIR_LITERAL_NULLPTR)
			type.kind = LOWIR_TYPE_PTR;
	}
	else if(operand.kind == LOWIR_OPERAND_GLOBAL &&
	        info.is_function(operand.name))
		type.kind = LOWIR_TYPE_PTR;
	return type;
}

// Loads a value already in storage at `location` into register `file`,
// using the type's operand width; sub-32-bit loads zero the register
// first because move8/move16 keep the upper bits.
void LowIRCY86Emitter::load_from_location(char file, const string & location,
                                          const LowIRType & type)
{
	long bits = type.operand_bits();
	if(bits == 64 || bits == 0 || bits == 80)
		ins("move64 " + reg(file, 64) + " " + location);
	else if(bits == 32)
		ins("move32 " + reg(file, 32) + " " + location);
	else
	{
		ins("move64 " + reg(file, 64) + " 0");
		ins("move" + std::to_string(bits) + " " + reg(file, bits) + " " +
		    location);
	}
}

// Loads a scalar operand into register `file`. Literals keep their
// source spelling; slot operands decay to their address; globals load
// their contents while function symbols load their label value.
void LowIRCY86Emitter::load_scalar(char file, const LowIROperand & operand,
                                   const LowIRType & type)
{
	switch(operand.kind)
	{
	case LOWIR_OPERAND_LITERAL:
		if(operand.literal_class == LOWIR_LITERAL_F32)
			ins("move32 " + reg(file, 32) + " " + literal_text(operand));
		else
			ins("move64 " + reg(file, 64) + " " + literal_text(operand));
		break;
	case LOWIR_OPERAND_TEMP:
		load_from_location(file, home_ref(frame.home(operand.name).offset),
		                   type);
		break;
	case LOWIR_OPERAND_SLOT:
		ins("isub64 " + reg(file, 64) + " bp " +
		    std::to_string(frame.home(operand.name).offset));
		break;
	case LOWIR_OPERAND_GLOBAL:
		if(info.is_function(operand.name))
			ins("move64 " + reg(file, 64) + " " +
			    function_label(operand.name));
		else
			load_from_location(file, "[" + global_label(operand.name) + "]",
			                   type);
		break;
	default:
		throw std::runtime_error("cannot load empty operand");
	}
}

// Stores the value register (x) to `location` with the type's width.
void LowIRCY86Emitter::store_to_location(const string & location,
                                         const LowIRType & type)
{
	long bits = type.operand_bits();
	if(bits == 0 || bits == 80)
		bits = 64;
	ins("move" + std::to_string(bits) + " " + location + " " +
	    reg('x', bits));
}

void LowIRCY86Emitter::store_result_home(const string & temp,
                                         const LowIRType & type)
{
	store_to_location(home_ref(frame.home(temp).offset), type);
}

// Materializes the address of an addressable operand into x64: slots and
// object-valued temporaries use their frame home, globals and functions
// use their labels.
void LowIRCY86Emitter::address_to_x64(const LowIROperand & operand)
{
	switch(operand.kind)
	{
	case LOWIR_OPERAND_SLOT:
	case LOWIR_OPERAND_TEMP:
		ins("isub64 x64 bp " +
		    std::to_string(frame.home(operand.name).offset));
		break;
	case LOWIR_OPERAND_GLOBAL:
		if(info.is_function(operand.name))
			ins("move64 x64 " + function_label(operand.name));
		else
			ins("move64 x64 " + global_label(operand.name));
		break;
	default:
		throw std::runtime_error("operand has no address");
	}
}

// Zeroes the 6 padding bytes after a fresh 10-byte f80 write at
// [bp-offset]; every move80/f80-arithmetic result is padded this way.
void LowIRCY86Emitter::f80_pad(long offset)
{
	ins("move64 z64 0");
	ins("move32 [bp-" + std::to_string(offset - 10) + "] z32");
	ins("move16 [bp-" + std::to_string(offset - 14) + "] z16");
}

// Copies the 16 bytes addressed by x64 into the scratch slot.
void LowIRCY86Emitter::f80_words_to_scratch(long scratch_offset)
{
	ins("move64 z64 [x64]");
	ins("move64 " + home_ref(scratch_offset) + " z64");
	ins("move64 z64 [x64+8]");
	ins("move64 " + home_high_ref(scratch_offset) + " z64");
}

// Materializes an f80 operand into a 16-byte bp-relative slot (scratch
// or a call-argument literal home): literals via move80 plus padding,
// stored values via their address.
void LowIRCY86Emitter::f80_value_to_scratch(const LowIROperand & operand,
                                            long scratch_offset)
{
	if(operand.is_literal())
	{
		ins("move80 " + home_ref(scratch_offset) + " " +
		    literal_text(operand));
		f80_pad(scratch_offset);
		return;
	}
	if(operand.kind == LOWIR_OPERAND_TEMP &&
	   !operand_is_object_value(operand))
	{
		// A pointer-valued temporary: dereference it.
		ins("move64 x64 " + home_ref(frame.home(operand.name).offset));
	}
	else
		address_to_x64(operand);
	f80_words_to_scratch(scratch_offset);
}

void LowIRCY86Emitter::f80_scratch_to_home(long scratch_offset,
                                           long home_offset)
{
	ins("move64 z64 " + home_ref(scratch_offset));
	ins("move64 " + home_ref(home_offset) + " z64");
	ins("move64 z64 " + home_high_ref(scratch_offset));
	ins("move64 " + home_high_ref(home_offset) + " z64");
}

// Writes the scratch slot's 16 bytes through the pointer in x64.
void LowIRCY86Emitter::f80_scratch_through_pointer(long scratch_offset)
{
	ins("move64 z64 " + home_ref(scratch_offset));
	ins("move64 [x64] z64");
	ins("move64 z64 " + home_high_ref(scratch_offset));
	ins("move64 [x64+8] z64");
}

// Routes a floating conversion source into f80 scratch: f32/f64 through
// their widening ops, f80 values by copy. Integer sources are widened in
// emit_convert, which knows the operator's signedness.
void LowIRCY86Emitter::convert_source_to_scratch(
	const LowIROperand & operand, const LowIRType & source, long scratch)
{
	if(source.is_f80())
	{
		f80_value_to_scratch(operand, scratch);
		return;
	}
	load_scalar('x', operand, source);
	long bits = source.operand_bits();
	string convert_op;
	if(source.kind == LOWIR_TYPE_F32)
		convert_op = "f32convf80";
	else if(source.kind == LOWIR_TYPE_F64)
		convert_op = "f64convf80";
	else
		throw std::runtime_error("floating conversion source expected");
	ins(convert_op + " " + home_ref(scratch) + " " + reg('x', bits));
	f80_pad(scratch);
}
