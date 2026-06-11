#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

#include "lowering/lower_program.h"
#include "sema/scope.h"
#include "sema/sem_node.h"
#include "sema/type.h"

// PA14 function-body lowering: one FunctionLowerer turns one resolved
// SN_FUNCTION_DEFINITION into LowIR text. Statement lowering lives in
// lower_function.cpp; expression lowering in lower_expr.cpp; both
// share the block/slot/temp state here.

// A lowered expression value: the operand spelling plus the typed
// immediate facts the conversion rules need.
struct LowerValue
{
	LowerValue() : imm_int(false), imm_null(false), imm_float(false) {}

	string text;   // operand spelling ("%t1", "$x", "3", "1.5f")
	TypePtr type;  // C++ type of the value
	bool imm_int;  // integral immediate with `value`
	ConstValue value;
	bool imm_null;   // null pointer immediate (not yet materialized)
	bool imm_float;  // floating literal immediate
};

// Conversion spelling contexts (see plan: immediates canonicalize in
// copy-initialization, spell out in operand positions, and explicit
// casts always spell their instruction).
enum ELowerConvertContext
{
	LCC_INIT,     // variable init / argument / return / conditional arm
	LCC_OPERAND,  // usual-arithmetic / assignment operand conversion
	LCC_CAST      // explicit cast
};

class FunctionLowerer
{
public:
	FunctionLowerer(LowerProgram& program, const SemNode& definition,
	                const LowFunctionInfo& info);

	// The complete `function ... { ... }` definition text.
	string Lower();

private:
	struct Block
	{
		Block() : terminated(false) {}

		string label;
		vector<string> lines;
		bool terminated;
	};

	struct ParamInfo
	{
		string low_name;
		string type_text;
		bool by_reference;
	};

	// --- shared state helpers (lower_function.cpp) ---
	string Header() const;
	void EmitParameterStores();
	string NewTemp();
	string NewLabel(const string& prefix);
	void OpenBlock(const string& label);
	void Emit(const string& line);
	void Terminate(const string& line);
	void ReferenceLabel(const string& label);
	void CloseInto(const string& label);
	string AddSlot(const Scope* scope, const string& name,
	               const string& type_text);
	string AddMatSlot(const string& kind, const string& type_text);
	string SlotRef(const Scope* scope, const string& name) const;
	string Render() const;
	void TerminateOpenEnd();

	// --- statements (lower_function.cpp) ---
	void LowerStatementList(const vector<SemNodePtr>& items, size_t from);
	void LowerStatement(const SemNode& node);
	void LowerLocalDeclaration(const SemNode& node);
	void LowerLocalVariable(const SemNode& node);
	void LowerLocalArrayInit(const SemNode& node, const string& slot,
	                         const TypePtr& array);
	void LowerReturn(const SemNode& node);
	void LowerIf(const SemNode& node);
	void LowerWhile(const SemNode& node);
	void LowerDo(const SemNode& node);
	void LowerFor(const SemNode& node);
	void LowerSwitch(const SemNode& node);
	void ScanSwitchLabels(const SemNode& node, vector<string>& arms,
	                      string& default_label);
	void LowerGoto(const SemNode& node);
	void LowerLabelStatement(const SemNode& node);
	string GotoLabel(const string& name);
	void LowerConditionInto(const SemNode& condition,
	                        const string& true_label,
	                        const string& false_label);

	// --- expressions (lower_expr.cpp) ---
	LowerValue LowerValueExpr(const SemNode& node);
	string LowerValueAs(const SemNode& node, const TypePtr& dest,
	                    ELowerConvertContext context);
	LowerValue ConvertValue(LowerValue value, const TypePtr& dest,
	                        ELowerConvertContext context);
	LowerValue ConvertToBool(LowerValue value, const TypePtr& source,
	                         const TypePtr& target);
	LowerValue ConvertIntegralImmediate(LowerValue value,
	                                    const TypePtr& source,
	                                    const TypePtr& target);
	string LowerAddressExpr(const SemNode& node);
	string LowerPointerOperand(const SemNode& node);
	string LowerPointerCmpOperand(const SemNode& node);
	void LowerCondition(const SemNode& node, const string& true_label,
	                    const string& false_label);
	void BranchOnValue(const SemNode& node, const string& true_label,
	                   const string& false_label);
	void LowerEffect(const SemNode& node);
	LowerValue LowerCall(const SemNode& node);
	string LowerCallArgument(const SemNode& node, const TypePtr& param);
	string LowerReferenceArgument(const SemNode& node,
	                              const TypePtr& referee);
	LowerValue LowerComparison(const SemNode& node);
	LowerValue LowerBinary(const SemNode& node);
	LowerValue LowerLogicalValue(const SemNode& node);
	LowerValue LowerUnary(const SemNode& node);
	LowerValue LowerIncDec(const SemNode& node, bool prefix);
	LowerValue LowerAssignment(const SemNode& node);
	LowerValue LowerCompoundAssignment(const SemNode& node,
	                                   const LowerValue& target);
	LowerValue LowerConditionalValue(const SemNode& node);
	string LowerConditionalAddress(const SemNode& node);
	LowerValue LowerIdValue(const SemNode& node);
	LowerValue LowerLiteralValue(const SemNode& node);
	string PointerStep(const string& base, const LowerValue& count,
	                   const TypePtr& element, bool negative);
	LowerValue MaterializeTruth(const LowerValue& value);
	string MaterializeNull();
	// Storage of a directly addressable lvalue ("$x"/"@g"), or "" when
	// the lvalue needs an address computation.
	string DirectStorage(const SemNode& node);
	const ScopeBinding* EntityBinding(const SemNode& node) const;

	// --- PA15 object model (lower_member.cpp) ---
	// The address of a member lvalue: base-subobject hops, the field
	// projection, and the indirection of reference members (skipped
	// when the store binds the reference itself).
	string MemberAddress(const SemNode& node, bool skip_ref_load = false);
	LowerValue LowerMemberValue(const SemNode& node);
	LowerValue LowerBitFieldValue(const SemNode& node);
	LowerValue LowerBitFieldAssignment(const SemNode& node);
	LowerValue LowerMemberAssignment(const SemNode& node);
	void LowerClassLocal(const SemNode& node);
	void LowerConstructorCall(const SemNode& action,
	                          const string& this_text);
	// A class temporary: a fresh object slot plus its constructor run;
	// returns the address temp ("%tN").
	string MaterializeTemporary(const SemNode& action, const char* kind);
	string ClassArrayElement(const string& base, const LowerValue& index,
	                         const TypePtr& element);

	// --- PA15 lifetime (lower_member.cpp) ---
	void PushCleanupScope();
	void PopCleanupScope(bool emit);
	void RegisterCleanup(const vector<const SemNode*>& actions);
	// Emits the destructor actions of the scopes at depth >= `from`,
	// innermost scope first; later objects destroy before earlier ones,
	// array elements in their recorded order.
	void EmitCleanupsFrom(size_t from);
	bool HaveCleanups() const;
	void CloseEhRegion();

	LowerProgram& program_;
	const SemNode& def_;
	const LowFunctionInfo& info_;
	TypePtr return_type_;

	vector<Block> blocks_;
	set<string> referenced_;
	vector<ParamInfo> params_;
	set<string> param_names_;
	vector<pair<string, string>> slots_;  // (name, type) print order
	map<pair<const void*, string>, string> slot_map_;
	map<string, int> slot_base_counts_;
	int temp_counter_;
	int label_counter_;
	int mat_counter_;
	vector<string> break_stack_;
	vector<string> continue_stack_;
	map<const SemNode*, string> case_labels_;
	map<string, string> goto_labels_;

	// --- PA15 lifetime state ---
	// Cleanup scopes parallel the lowered compound statements; each
	// scope holds per-object action groups in declaration order.
	vector<vector<vector<const SemNode*>>> cleanup_scopes_;
	vector<size_t> break_cleanup_;
	vector<size_t> continue_cleanup_;
	// The open unwind-dispatch region of the current full-expression.
	bool eh_open_;
	string eh_dispatch_;
	string eh_end_;
	// Lifetime-machinery calls (constructor actions, cleanups) never
	// open unwind regions.
	bool in_lifetime_action_;
};
