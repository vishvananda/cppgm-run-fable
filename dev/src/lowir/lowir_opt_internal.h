#pragma once

// Shared internals of the PA37 LowIR optimizer (lowir_opt*.cpp). The
// passes communicate through OptFunction, a per-function view over the
// parsed LowIRProgram: block index, normal/exceptional flow facts, and
// temp definition sites. Structural passes rebuild the view.

#include <map>
#include <set>
#include <string>
#include <vector>

#include "lowir/lowir_program.h"

using std::set;

struct LowIROptContext;

// Location of one instruction: block index and instruction index.
struct OptInsRef
{
	size_t block = 0;
	size_t index = 0;
};

struct OptFunction
{
	LowIRFunction * fn = nullptr;
	LowIROptContext * context = nullptr;
	bool optimizable = false;      // single-def temps, sane structure

	map<string, size_t> block_index;
	map<string, OptInsRef> temp_defs;     // result temp -> def site
	set<string> param_names;

	// Exception-structure facts (ComputeEhShape):
	vector<int> depth_in;          // region depth at block entry (-1 unset)
	vector<bool> handler_target;   // block is an eh_try/eh_cleanup target
	vector<bool> handler_context;  // reachable from a handler target

	LowIRBlock & block(size_t b) { return fn->blocks[b]; }
	size_t block_count() const { return fn->blocks.size(); }
};

// Program-wide optimization state shared across rounds.
struct LowIROptContext
{
	LowIRProgram * program = nullptr;
	int level = 1;
	map<string, const LowIRFunction *> functions_by_name;
	map<string, size_t> inline_counters;   // caller name -> next site id
	set<string> inline_cycle;              // functions in call-graph cycles

	void RefreshFunctionIndex();
	void RefreshInlineCycles();
	const LowIRFunction * FindFunction(const string & name) const;
};

// Builds the per-function view; sets optimizable=false when the body
// breaks the single-definition temp discipline the passes rely on.
void BuildOptFunction(OptFunction & view, LowIRFunction & fn,
                      LowIROptContext & context);

// Recomputes exception-structure facts (depth_in / handler flags).
void ComputeEhShape(OptFunction & view);

// --- flow helpers -----------------------------------------------------

// Successor labels of a terminator (empty for return/throw/resume).
void TerminatorTargets(const LowIRInstruction & ins,
                       vector<string> & targets);

// Whether an instruction may write memory (barrier for local
// store-to-load forwarding).
bool MayWriteMemory(const LowIRInstruction & ins);

// Whether a call may transfer to an exception handler, judged from the
// callee's spelled metadata only.
bool CallMayUnwind(const LowIROptContext & context,
                   const LowIRInstruction & ins);

// Whether an instruction may transfer to a handler (throw/resume or a
// may-unwind call).
bool MayTransferToHandler(const LowIROptContext & context,
                          const LowIRInstruction & ins);

// --- operand helpers --------------------------------------------------

bool SameOperand(const LowIROperand & a, const LowIROperand & b);

// Parses an integer literal operand into 64-bit two's-complement bits;
// false for floats/nullptr or unparsable spellings.
bool OperandIntValue(const LowIROperand & operand,
                     unsigned long long & bits);

// Parses any numeric literal (int or float) into a long double value.
bool OperandFloatValue(const LowIROperand & operand, long double & value);

// Canonical folded-integer literal for a result type (signed decimal
// spelling for signed types, unsigned for unsigned types).
LowIROperand MakeIntLiteral(const LowIRType & type,
                            unsigned long long bits);

// Wraps 64-bit two's-complement bits to the width of `type`
// (sign-extending so signed reads see the canonical value).
unsigned long long WrapIntToType(const LowIRType & type,
                                 unsigned long long bits);

// Substitution map: temp name -> replacement operand. Applies to every
// value-operand position; returns false (no changes committed) when a
// replacement literal would land in a storage/callee position.
typedef map<string, LowIROperand> OptSubstitution;
bool ApplySubstitution(LowIRFunction & fn, const OptSubstitution & subst);

// --- pass entry points ------------------------------------------------

// Executable-edge value pass: constant folding, algebraic identities,
// copy/constant propagation, boolean compare cleanup, reassociation,
// terminator folding; at level 2 also promotable-slot value tracking.
bool FoldAndPropagate(OptFunction & view);

// Executable-edge pure-expression reuse (CSE).
bool ReuseExpressions(OptFunction & view);

// Unreachable-block removal, jump threading, pair merging.
bool CleanupControlFlow(OptFunction & view);

// Dead code: unused pure temps, removable calls, dead slot traffic,
// same-block slot store forwarding, unused slots; at level 2 dead
// stores to promoted slots.
bool EliminateDeadCode(OptFunction & view);

// eh_try/eh_end removal in unwind=no functions whose protected region
// cannot transfer to the handler.
bool StripNoOpEh(OptFunction & view);

// Inline expansion of eligible direct calls; returns whether any call
// was expanded.
bool InlineCalls(OptFunction & view);
