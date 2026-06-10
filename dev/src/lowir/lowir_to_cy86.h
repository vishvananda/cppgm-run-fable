#pragma once

#include "lowir/lowir_frame.h"
#include "lowir/lowir_program.h"
#include "lowir/lowir_validate.h"

// PA13 LowIR-to-CY86 translation. Templates are fixed per instruction so
// the text is deterministic and adding instructions later never reshapes
// existing output (see pa13/plan.md for the emission contract).
//
// Entry point used by the lowir2cy86 driver:
string TranslateLowIRProgramToCY86(const LowIRProgram & program,
                                   const LowIRProgramInfo & info);

// Largest move width (in bytes) usable for the remaining span of a
// chunked bulk-memory copy or zero fill.
inline long LowIRChunkBytes(long remaining)
{
	if(remaining >= 8) return 8;
	if(remaining >= 4) return 4;
	if(remaining >= 2) return 2;
	return 1;
}

// Shared emitter context. The implementation is split across
// lowir_emit_values.cpp (writer and operand/f80 helpers),
// lowir_emit_inst.cpp (per-instruction templates), and
// lowir_emit_program.cpp (program/function/global structure).
struct LowIRCY86Emitter
{
	const LowIRProgram & program;
	const LowIRProgramInfo & info;
	string out;
	long label_counter = 0;
	bool uses_eh = false;

	// Current function state.
	const LowIRFunction * function = nullptr;
	LowIRFrame frame;

	LowIRCY86Emitter(const LowIRProgram & p, const LowIRProgramInfo & i)
		: program(p), info(i) {}

	// --- lowir_emit_values.cpp ---
	void ins(const string & text);
	void label(const string & name);
	void blank();
	string reg(char file, long bits) const;
	string home_ref(long offset) const;             // "[bp-N]"
	string home_high_ref(long offset) const;        // "[bp-(N-8)]"
	string global_label(const string & name) const; // "g__name"
	string function_label(const string & name) const;
	string block_label(const string & block) const; // current function
	string literal_text(const LowIROperand & operand) const;
	string fresh_label(const string & stem);
	const LowIRHome * find_home(const string & name) const;
	bool operand_is_object_value(const LowIROperand & operand) const;
	LowIRType operand_value_type(const LowIROperand & operand) const;
	void load_scalar(char file, const LowIROperand & operand,
	                 const LowIRType & type);
	void load_from_location(char file, const string & location,
	                        const LowIRType & type);
	void store_to_location(const string & location, const LowIRType & type);
	void store_result_home(const string & temp, const LowIRType & type);
	void address_to_x64(const LowIROperand & operand);
	void f80_pad(long offset);
	void f80_words_to_scratch(long scratch_offset);  // from address in x64
	void f80_value_to_scratch(const LowIROperand & operand,
	                          long scratch_offset);
	void f80_scratch_to_home(long scratch_offset, long home_offset);
	void f80_scratch_through_pointer(long scratch_offset);
	void convert_source_to_scratch(const LowIROperand & operand,
	                               const LowIRType & source, long scratch);

	// --- lowir_emit_inst.cpp ---
	void emit_instruction(const LowIRInstruction & ins);
	void emit_const_copy(const LowIRInstruction & ins);
	void emit_addr(const LowIRInstruction & ins);
	void emit_load(const LowIRInstruction & ins);
	void emit_store(const LowIRInstruction & ins);
	void emit_atomic_load(const LowIRInstruction & ins);
	void emit_atomic_store(const LowIRInstruction & ins);
	void emit_atomic_exchange(const LowIRInstruction & ins);
	void emit_atomic_compare_exchange(const LowIRInstruction & ins);
	void emit_atomic_add_fetch(const LowIRInstruction & ins);
	void emit_index(const LowIRInstruction & ins);
	void emit_unary(const LowIRInstruction & ins);
	void emit_binary(const LowIRInstruction & ins);
	void emit_cmp(const LowIRInstruction & ins);
	void emit_convert(const LowIRInstruction & ins);
	void emit_call(const LowIRInstruction & ins);
	void emit_copyobj(const LowIRInstruction & ins);
	void emit_zeroinit(const LowIRInstruction & ins);
	void emit_eh_push(const LowIRInstruction & ins);
	void emit_eh_end();
	void emit_eh_dispatch();
	void emit_throw(const LowIRInstruction & ins);
	void emit_exception(const LowIRInstruction & ins);
	void emit_branch(const LowIRInstruction & ins);
	void emit_switch(const LowIRInstruction & ins);
	void emit_return(const LowIRInstruction & ins);

	// --- lowir_emit_program.cpp ---
	void emit_start_block();
	void emit_function(const LowIRFunction & function);
	void emit_parameter_captures();
	void emit_eh_runtime_function();
	void emit_global(const LowIRGlobal & global);
	void emit_data_item(const LowIRDataItem & item, long & offset);
	void emit_f80_data(const LowIROperand & literal);
	string address_init_text(const string & symbol, bool has_addend,
	                         long addend) const;
	void emit_eh_runtime_globals();
	string run();
};
