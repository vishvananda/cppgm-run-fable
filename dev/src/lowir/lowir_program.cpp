#include "lowir/lowir_program.h"

#include <cstdlib>
#include <stdexcept>

using std::runtime_error;

long LowIRType::integer_bits() const
{
	switch(kind)
	{
	case LOWIR_TYPE_I1: return 1;
	case LOWIR_TYPE_I8: case LOWIR_TYPE_U8: return 8;
	case LOWIR_TYPE_I16: case LOWIR_TYPE_U16: return 16;
	case LOWIR_TYPE_I32: case LOWIR_TYPE_U32: return 32;
	case LOWIR_TYPE_I64: return 64;
	default: return 0;
	}
}

long LowIRType::operand_bits() const
{
	switch(kind)
	{
	case LOWIR_TYPE_I1: case LOWIR_TYPE_I8: case LOWIR_TYPE_U8:
		return 8;
	case LOWIR_TYPE_I16: case LOWIR_TYPE_U16:
		return 16;
	case LOWIR_TYPE_I32: case LOWIR_TYPE_U32: case LOWIR_TYPE_F32:
		return 32;
	case LOWIR_TYPE_I64: case LOWIR_TYPE_F64: case LOWIR_TYPE_PTR:
		return 64;
	case LOWIR_TYPE_F80:
		return 80;
	default:
		return 0;
	}
}

long LowIRType::element_bytes() const
{
	switch(kind)
	{
	case LOWIR_TYPE_F80: return 16;
	case LOWIR_TYPE_OBJ: return obj_bytes;
	default: return operand_bits() / 8;
	}
}

long LowIRType::home_bytes() const
{
	if(kind == LOWIR_TYPE_VOID)
		return 0;
	if(kind == LOWIR_TYPE_F80)
		return 16;
	if(kind == LOWIR_TYPE_OBJ)
		return (obj_bytes + 7) / 8 * 8;
	return 8;
}

bool LowIRMetadata::has(const string & key) const
{
	for(size_t i = 0; i < items.size(); ++i)
		if(items[i].first == key)
			return true;
	return false;
}

string LowIRMetadata::find(const string & key) const
{
	for(size_t i = 0; i < items.size(); ++i)
		if(items[i].first == key)
			return items[i].second;
	return "";
}

LowIRType ParseLowIRType(const string & spelling)
{
	LowIRType type;
	if(spelling == "void") type.kind = LOWIR_TYPE_VOID;
	else if(spelling == "i1") type.kind = LOWIR_TYPE_I1;
	else if(spelling == "i8") type.kind = LOWIR_TYPE_I8;
	else if(spelling == "u8") type.kind = LOWIR_TYPE_U8;
	else if(spelling == "i16") type.kind = LOWIR_TYPE_I16;
	else if(spelling == "u16") type.kind = LOWIR_TYPE_U16;
	else if(spelling == "i32") type.kind = LOWIR_TYPE_I32;
	else if(spelling == "u32") type.kind = LOWIR_TYPE_U32;
	else if(spelling == "i64") type.kind = LOWIR_TYPE_I64;
	else if(spelling == "f32") type.kind = LOWIR_TYPE_F32;
	else if(spelling == "f64") type.kind = LOWIR_TYPE_F64;
	else if(spelling == "f80") type.kind = LOWIR_TYPE_F80;
	else if(spelling == "ptr") type.kind = LOWIR_TYPE_PTR;
	else if(spelling.compare(0, 4, "obj<") == 0 &&
	        spelling.size() > 5 && spelling[spelling.size() - 1] == '>')
	{
		string span = spelling.substr(4, spelling.size() - 5);
		size_t split = span.find('x');
		if(split == string::npos || split == 0 || split + 1 == span.size())
			throw runtime_error("malformed object type: " + spelling);
		char * end = nullptr;
		const string bytes_text = span.substr(0, split);
		const string align_text = span.substr(split + 1);
		long bytes = strtol(bytes_text.c_str(), &end, 10);
		if(*end != '\0')
			throw runtime_error("malformed object size: " + spelling);
		long align = strtol(align_text.c_str(), &end, 10);
		if(*end != '\0')
			throw runtime_error("malformed object alignment: " + spelling);
		if(bytes <= 0 || align <= 0)
			throw runtime_error("non-positive object span: " + spelling);
		type.kind = LOWIR_TYPE_OBJ;
		type.obj_bytes = bytes;
		type.obj_align = align;
	}
	else
		throw runtime_error("unknown LowIR type: " + spelling);
	return type;
}
