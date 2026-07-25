#include "lowir/lowir_dump.h"

#include <stdexcept>

using std::ostream;
using std::to_string;

string LowIRTypeText(const LowIRType & type)
{
	switch(type.kind)
	{
	case LOWIR_TYPE_VOID: return "void";
	case LOWIR_TYPE_I1: return "i1";
	case LOWIR_TYPE_I8: return "i8";
	case LOWIR_TYPE_U8: return "u8";
	case LOWIR_TYPE_I16: return "i16";
	case LOWIR_TYPE_U16: return "u16";
	case LOWIR_TYPE_I32: return "i32";
	case LOWIR_TYPE_U32: return "u32";
	case LOWIR_TYPE_I64: return "i64";
	case LOWIR_TYPE_I128: return "i128";
	case LOWIR_TYPE_F32: return "f32";
	case LOWIR_TYPE_F64: return "f64";
	case LOWIR_TYPE_F80: return "f80";
	case LOWIR_TYPE_PTR: return "ptr";
	case LOWIR_TYPE_OBJ:
		return "obj<" + to_string(type.obj_bytes) + "x" +
		       to_string(type.obj_align) + ">";
	}
	throw std::runtime_error("unknown LowIR type kind");
}

string LowIROperandText(const LowIROperand & operand)
{
	switch(operand.kind)
	{
	case LOWIR_OPERAND_TEMP: return "%" + operand.name;
	case LOWIR_OPERAND_SLOT: return "$" + operand.name;
	case LOWIR_OPERAND_GLOBAL: return "@" + operand.name;
	case LOWIR_OPERAND_LITERAL:
		return (operand.negated ? "-" : "") + operand.literal;
	case LOWIR_OPERAND_NONE: break;
	}
	throw std::runtime_error("unprintable LowIR operand");
}

namespace {

string metadata_text(const LowIRMetadata & metadata)
{
	if(metadata.items.empty())
		return "";
	string out = " [";
	for(size_t i = 0; i < metadata.items.size(); i++)
	{
		if(i)
			out += ", ";
		out += metadata.items[i].first + "=" + metadata.items[i].second;
	}
	return out + "]";
}

string param_list_text(const vector<LowIRParam> & params)
{
	string out = "(";
	for(size_t i = 0; i < params.size(); i++)
	{
		if(i)
			out += ", ";
		out += "%" + params[i].name + " : " +
			LowIRTypeText(params[i].type) +
			metadata_text(params[i].metadata);
	}
	return out + ")";
}

string span_text(const LowIRInstruction & ins)
{
	return to_string(ins.span_bytes) + "x" + to_string(ins.span_align);
}

string call_text(const LowIRInstruction & ins)
{
	string out = "call " + LowIRTypeText(ins.type) + " " +
		(ins.callee_is_temp ? "%" : "@") + ins.callee + "(";
	for(size_t i = 0; i < ins.operands.size(); i++)
	{
		if(i)
			out += ", ";
		out += LowIROperandText(ins.operands[i]);
	}
	out += ")";
	if(ins.signature.present)
	{
		out += " as " + param_list_text(ins.signature.params) + " -> " +
			LowIRTypeText(ins.signature.return_type) +
			metadata_text(ins.signature.metadata);
	}
	return out;
}

string operands_text(const LowIRInstruction & ins)
{
	string out;
	for(size_t i = 0; i < ins.operands.size(); i++)
	{
		if(i)
			out += ", ";
		out += LowIROperandText(ins.operands[i]);
	}
	return out;
}

string eh_marker_text(const LowIRInstruction & ins)
{
	string out = ins.operation;
	if(ins.operation == "eh_catch")
	{
		out += " @" + ins.eh_types[0];
		if(ins.eh_selector)
			out += ", " + to_string(ins.eh_selector);
	}
	else if(ins.operation == "eh_filter")
	{
		for(size_t i = 0; i < ins.eh_types.size(); i++)
			out += (i ? ", @" : " @") + ins.eh_types[i];
	}
	else if(ins.operation == "eh_catch_all")
	{
		if(ins.eh_selector)
			out += ", " + to_string(ins.eh_selector);
	}
	return out;
}

}  // namespace

string LowIRInstructionText(const LowIRInstruction & ins)
{
	string head = ins.result.empty() ? "" : "%" + ins.result + " = ";
	switch(ins.opcode)
	{
	case LOWIR_INS_CONST:
		return head + "const " + LowIRTypeText(ins.type) + " " +
			operands_text(ins);
	case LOWIR_INS_COPY:
		return head + "copy " + LowIRTypeText(ins.type) + " " +
			operands_text(ins);
	case LOWIR_INS_ADDR:
		return head + "addr " + operands_text(ins);
	case LOWIR_INS_LOAD:
		return head + "load " + LowIRTypeText(ins.type) + " " +
			operands_text(ins);
	case LOWIR_INS_STORE:
		return "store " + LowIRTypeText(ins.type) + " " +
			operands_text(ins);
	case LOWIR_INS_ATOMIC_LOAD:
		return head + "atomic_load " + LowIRTypeText(ins.type) + " " +
			operands_text(ins) + ", " + to_string(ins.atomic_order);
	case LOWIR_INS_ATOMIC_STORE:
		return "atomic_store " + LowIRTypeText(ins.type) + " " +
			operands_text(ins) + ", " + to_string(ins.atomic_order);
	case LOWIR_INS_ATOMIC_EXCHANGE:
		return head + "atomic_exchange " + LowIRTypeText(ins.type) +
			" " + operands_text(ins) + ", " +
			to_string(ins.atomic_order);
	case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
		return head + "atomic_compare_exchange " +
			LowIRTypeText(ins.type) + " " + operands_text(ins) + ", " +
			to_string(ins.atomic_order) + ", " +
			to_string(ins.atomic_failure_order);
	case LOWIR_INS_ATOMIC_ADD_FETCH:
		return head + "atomic_add_fetch " + LowIRTypeText(ins.type) +
			" " + operands_text(ins) + ", " +
			to_string(ins.atomic_order);
	case LOWIR_INS_ATOMIC_THREAD_FENCE:
		return "atomic_thread_fence " + to_string(ins.atomic_order);
	case LOWIR_INS_ATOMIC_SIGNAL_FENCE:
		return "atomic_signal_fence " + to_string(ins.atomic_order);
	case LOWIR_INS_INDEX:
		return head + "index " + LowIRTypeText(ins.type) +
			metadata_text(ins.metadata) + " " + operands_text(ins);
	case LOWIR_INS_UNARY:
		return head + "unary " + ins.operation + " " +
			LowIRTypeText(ins.type) + " " + operands_text(ins);
	case LOWIR_INS_BINARY:
		return head + "binary " + ins.operation + " " +
			LowIRTypeText(ins.type) + " " + operands_text(ins);
	case LOWIR_INS_CMP:
		return head + "cmp " + ins.operation + " " +
			LowIRTypeText(ins.type) + " " + operands_text(ins);
	case LOWIR_INS_CONVERT:
		return head + "convert " + ins.operation + " " +
			LowIRTypeText(ins.type) + " " + LowIRTypeText(ins.type2) +
			" " + operands_text(ins);
	case LOWIR_INS_CALL:
		return head + call_text(ins);
	case LOWIR_INS_COPYOBJ:
		return "copyobj " + span_text(ins) + " " + operands_text(ins);
	case LOWIR_INS_ZEROINIT:
		return "zeroinit " + span_text(ins) + " " + operands_text(ins);
	case LOWIR_INS_EXCEPTION:
		return head + ins.operation + " " + LowIRTypeText(ins.type);
	case LOWIR_INS_EH_TRY:
		return "eh_try ^" + ins.block_targets[0];
	case LOWIR_INS_EH_CLEANUP:
		return "eh_cleanup ^" + ins.block_targets[0];
	case LOWIR_INS_EH_END:
		return "eh_end";
	case LOWIR_INS_EH_MARKER:
		return eh_marker_text(ins);
	case LOWIR_INS_THROW:
		return "throw " + LowIRTypeText(ins.type) + " " +
			operands_text(ins);
	case LOWIR_INS_RESUME:
		return "resume";
	case LOWIR_INS_JUMP:
		return "jump ^" + ins.block_targets[0];
	case LOWIR_INS_BRANCH:
		return "branch " + operands_text(ins) + ", ^" +
			ins.block_targets[0] + ", ^" + ins.block_targets[1];
	case LOWIR_INS_SWITCH:
	{
		string out = "switch " + LowIROperandText(ins.operands[0]) +
			", ^" + ins.block_targets[0];
		for(size_t i = 0; i < ins.switch_values.size(); i++)
			out += ", " + LowIROperandText(ins.switch_values[i]) + ":^" +
				ins.block_targets[i + 1];
		return out;
	}
	case LOWIR_INS_RETURN:
	{
		string out = "return " + LowIRTypeText(ins.type);
		if(!ins.operands.empty())
			out += " " + LowIROperandText(ins.operands[0]);
		return out;
	}
	}
	throw std::runtime_error("unprintable LowIR instruction");
}

namespace {

// A definition carrying prefer_local=yes without an explicit binding
// prints binding=strong inserted before prefer_local (reference
// presentation parity).
LowIRMetadata function_definition_metadata(const LowIRFunction & function)
{
	LowIRMetadata metadata = function.metadata;
	if(!function.is_definition || metadata.has("binding") ||
	   metadata.find("prefer_local") != "yes")
		return metadata;
	for(size_t i = 0; i < metadata.items.size(); i++)
		if(metadata.items[i].first == "prefer_local")
		{
			metadata.items.insert(
				metadata.items.begin() + static_cast<long>(i),
				std::make_pair(string("binding"), string("strong")));
			break;
		}
	return metadata;
}

string function_header_text(const LowIRFunction & function)
{
	return (function.is_definition ? "function @" : "declare function @") +
		function.name + param_list_text(function.params) + " -> " +
		LowIRTypeText(function.return_type) +
		metadata_text(function_definition_metadata(function));
}

string data_item_text(const LowIRDataItem & item)
{
	if(item.kind == LOWIR_DATA_ZERO)
		return "zero " + to_string(item.zero_bytes);
	if(item.kind == LOWIR_DATA_ADDR)
	{
		string out = "ptr addr @" + item.symbol;
		if(item.has_addend)
			out += (item.addend < 0 ? " - " : " + ") +
				to_string(item.addend < 0 ? -item.addend : item.addend);
		return out;
	}
	return LowIRTypeText(item.type) + " " + LowIROperandText(item.value);
}

string global_text(const LowIRGlobal & global)
{
	string out = (global.is_definition ? "global @" : "declare global @") +
		global.name;
	if(global.has_type)
		out += " : " + LowIRTypeText(global.type);
	out += metadata_text(global.metadata);
	if(!global.is_definition)
		return out;
	out += " = ";
	switch(global.init)
	{
	case LOWIR_GLOBAL_ZERO:
		return out + "zero";
	case LOWIR_GLOBAL_SCALAR:
		return out + LowIROperandText(global.scalar);
	case LOWIR_GLOBAL_ADDR:
	{
		out += "addr @" + global.addr_symbol;
		if(global.has_addr_addend)
			out += (global.addr_addend < 0 ? " - " : " + ") +
				to_string(global.addr_addend < 0 ? -global.addr_addend
				                                 : global.addr_addend);
		return out;
	}
	case LOWIR_GLOBAL_STRUCTURED:
	{
		out += "{\n";
		for(size_t i = 0; i < global.items.size(); i++)
			out += "  " + data_item_text(global.items[i]) + "\n";
		return out + "}";
	}
	case LOWIR_GLOBAL_NONE:
		break;
	}
	throw std::runtime_error("unprintable LowIR global");
}

string function_body_text(const LowIRFunction & function)
{
	string out = function_header_text(function) + " {\n";
	for(size_t i = 0; i < function.slots.size(); i++)
		out += "  slot $" + function.slots[i].name + " : " +
			LowIRTypeText(function.slots[i].type) + "\n";
	for(size_t b = 0; b < function.blocks.size(); b++)
	{
		if(b || !function.slots.empty())
			out += "\n";
		const LowIRBlock & block = function.blocks[b];
		out += "  block ^" + block.label + ":\n";
		for(size_t i = 0; i < block.instructions.size(); i++)
			out += "    " +
				LowIRInstructionText(block.instructions[i]) + "\n";
	}
	return out + "}";
}

}  // namespace

void DumpLowIRProgram(const LowIRProgram & program, ostream & out)
{
	// The pa13/lowir.md presentation phases; a blank line separates one
	// phase's entries from the previous phase, except the alias tail.
	vector<string> sections[5];
	for(size_t i = 0; i < program.globals.size(); i++)
		if(!program.globals[i].is_definition)
			sections[0].push_back(global_text(program.globals[i]));
	for(size_t i = 0; i < program.functions.size(); i++)
		if(!program.functions[i].is_definition)
			sections[1].push_back(
				function_header_text(program.functions[i]));
	for(size_t i = 0; i < program.globals.size(); i++)
		if(program.globals[i].is_definition)
			sections[2].push_back(global_text(program.globals[i]));
	for(size_t i = 0; i < program.functions.size(); i++)
		if(program.functions[i].is_definition)
			sections[3].push_back(
				function_body_text(program.functions[i]));
	for(size_t i = 0; i < program.aliases.size(); i++)
		sections[4].push_back("alias object " +
			program.aliases[i].object_symbol + " = @" +
			program.aliases[i].target);

	bool first = true;
	for(int kind = 0; kind < 5; kind++)
	{
		bool separate = kind != 4;
		for(size_t i = 0; i < sections[kind].size(); i++)
		{
			if(!first && i == 0 && separate)
				out << "\n";
			out << sections[kind][i] << "\n";
			first = false;
		}
	}
}
