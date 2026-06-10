#include "lowir/lowir_frame.h"

#include <stdexcept>

namespace {

// The home type a defined temporary takes from its defining instruction.
LowIRType result_type(const LowIRInstruction & ins)
{
	LowIRType type;
	switch(ins.opcode)
	{
	case LOWIR_INS_ADDR:
	case LOWIR_INS_INDEX:
		type.kind = LOWIR_TYPE_PTR;
		return type;
	case LOWIR_INS_CMP:
	case LOWIR_INS_ATOMIC_COMPARE_EXCHANGE:
		type.kind = LOWIR_TYPE_I64;
		return type;
	default:
		return ins.type;
	}
}

bool mentions_f80(const LowIRType & type)
{
	return type.kind == LOWIR_TYPE_F80;
}

bool instruction_needs_scratch(const LowIRInstruction & ins)
{
	if(ins.opcode == LOWIR_INS_CONVERT)
		return true;
	if(mentions_f80(ins.type) || mentions_f80(ins.type2))
		return true;
	if(ins.signature.present)
	{
		if(mentions_f80(ins.signature.return_type))
			return true;
		for(size_t i = 0; i < ins.signature.params.size(); ++i)
			if(mentions_f80(ins.signature.params[i].type))
				return true;
	}
	return false;
}

}  // namespace

const LowIRHome & LowIRFrame::home(const string & name) const
{
	map<string, LowIRHome>::const_iterator found = values.find(name);
	if(found == values.end())
		throw std::runtime_error("no frame home for: " + name);
	return found->second;
}

LowIRFrame ComputeLowIRFrame(const LowIRFunction & function)
{
	LowIRFrame frame;
	long cursor = 0;
	bool scratch = false;

	LowIRType return_type = function.return_type;
	if(return_type.is_f80() || return_type.is_obj())
	{
		cursor += 8;
		frame.ret_pointer_offset = cursor;
	}
	scratch = scratch || mentions_f80(return_type);

	for(size_t i = 0; i < function.params.size(); ++i)
	{
		const LowIRParam & param = function.params[i];
		cursor += param.type.home_bytes();
		LowIRHome home;
		home.offset = cursor;
		home.type = param.type;
		frame.values[param.name] = home;
		scratch = scratch || mentions_f80(param.type);
	}
	for(size_t i = 0; i < function.slots.size(); ++i)
	{
		const LowIRSlot & slot = function.slots[i];
		cursor += slot.type.home_bytes();
		LowIRHome home;
		home.offset = cursor;
		home.type = slot.type;
		frame.values[slot.name] = home;
		scratch = scratch || mentions_f80(slot.type);
	}
	for(size_t b = 0; b < function.blocks.size(); ++b)
	{
		const LowIRBlock & block = function.blocks[b];
		for(size_t i = 0; i < block.instructions.size(); ++i)
		{
			const LowIRInstruction & ins = block.instructions[i];
			scratch = scratch || instruction_needs_scratch(ins);
			if(ins.result.empty() || frame.values.count(ins.result))
				continue;
			LowIRHome home;
			home.type = result_type(ins);
			cursor += home.type.home_bytes();
			home.offset = cursor;
			frame.values[ins.result] = home;
		}
	}

	frame.homes_size = cursor;
	frame.has_scratch = scratch;
	frame.frame_size = cursor + (scratch ? 64 : 0);
	return frame;
}
