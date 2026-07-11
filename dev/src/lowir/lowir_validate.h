#pragma once

#include "lowir/lowir_program.h"

// Resolved program-wide facts the validator derives and the emitter
// consumes: hook functions and symbol classification.
struct LowIRProgramInfo
{
	string entry_function;   // role=entry definition or legacy @main
	string init_function;    // role=init definition or legacy @__cppgm_init
	string fini_function;    // role=fini definition or legacy @__cppgm_fini
	map<string, const LowIRFunction *> functions;  // by @name
	map<string, const LowIRGlobal *> globals;      // by @name

	bool is_function(const string & name) const
	{
		return functions.count(name) != 0;
	}
};

// Checks the PA13 structural contract (see pa13/plan.md for the rule
// list) and resolves entry/init/fini hooks. Throws std::runtime_error on
// any violation; the driver reports EXIT_FAILURE.
// PA28 helper-only MIR dumps pass require_entry=false and omit the
// startup section instead of failing entry resolution.
LowIRProgramInfo ValidateLowIRProgram(const LowIRProgram & program,
                                      bool require_entry = true);
