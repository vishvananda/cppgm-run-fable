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
	// PA27: the value cannot be null (`this`), so a displaced-base
	// adjustment skips the null guard (4.10p3 protects null only).
	bool known_nonnull = false;
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
		// Pass metadata: "" / "reference" / "by_address" /
		// "indirect_result".
		string pass;
	};

	// --- PA27 hidden vbase-pointer state (lower_member.cpp helpers) ---
	// One declared parameter (or `this`) whose class carries virtual
	// bases: the carried hidden arguments by table index, and whether
	// the entries live in $name__pvbN slots (reference-to-class
	// parameters) or stay raw hidden registers.
	struct VBaseParamMap
	{
		VBaseParamMap() : cls(0), scope(0), slots(false) {}

		const ClassInfo* cls;
		// The parameter's declaring scope: lookups match it so a
		// shadowing local never rides the parameter's pointers (null
		// for `this`, which cannot be shadowed).
		const Scope* scope;
		bool slots;
		map<size_t, string> carried;  // table index -> "%__pvbptrK"
	};

	// Emits the pvb slot stores / static reconstructions of one
	// declared reference-to-class parameter and records its map.
	void EmitParamVBasePointers(size_t param_pos, size_t type_index,
	                            const SemNode& child);
	// Appends the callee's hidden trailing arguments to `arguments`.
	void AppendHiddenArguments(const vector<HiddenParam>& hidden,
	                           const vector<const SemNode*>& arg_nodes,
	                           const vector<string>& arg_texts,
	                           const SemNode* object_node,
	                           const string& object_text,
	                           string& arguments);
	// One lowered call's hidden row (registry signature for direct
	// callees, the type-only rule otherwise).
	void AppendCallHiddenArguments(const SemNode& node,
	                               const SemNode& callee,
	                               const TypePtr& fn_type, bool direct,
	                               bool pm_call,
	                               const vector<const SemNode*>& arg_nodes,
	                               const vector<string>& arg_texts,
	                               const string& object_text,
	                               string& arguments);
	// The declared argument row of one lowered call plus the hidden
	// trailing pointers.
	void LowerCallArgumentRow(const SemNode& node, const SemNode& callee,
	                          const TypePtr& fn_type, bool direct,
	                          bool pm_call, string& object_text,
	                          string& arguments);
	// PA27: a marked shared virtual-base action inside a base or
	// deleting entry drops (its callee stays demanded).
	bool SkipVBaseAction(const SemNode& node);
	// The address of `entry_cls`'s subobject for one hidden argument,
	// anchored on the argument expression (parameter maps, complete
	// locals, dynamic vpointer offsets, static fallback).
	string SupplyVBaseEntry(const ClassInfo& param_cls,
	                        const ClassInfo* entry_cls,
	                        const SemNode* arg_node,
	                        const string& arg_text);
	// The current function's own address of `entry_cls` (base-entry
	// forwarding or a static `this` projection).
	string OwnVBaseEntryAddress(const ClassInfo* entry_cls);
	// The current constructor/destructor's construction-table operand
	// for a base entry's __vtt argument.
	string SupplyVttArgument(const ClassInfo& callee_cls);

	// --- shared state helpers (lower_function.cpp) ---
	string Header() const;
	void EmitParameterStores();
	void LowerStaticGuard(const SemNode& node);
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
	// PA33 15.4p9 (host mode): a declared-noexcept body with a
	// may-unwind call arms a whole-body catch-all region whose pad
	// routes through the terminate trampoline.
	bool SubtreeMayUnwind(const SemNode& node) const;
	void ArmNoexceptTerminateRegion();
	void EmitNoexceptTerminateDispatch();

	// --- statements (lower_function.cpp) ---
	// PA16: picks the single top-level local every class-valued return
	// names (the return-slot-reuse form), if any.
	void ScanReturnSlotReuse();
	void CollectReturnLocals(const SemNode& node, bool& eligible,
	                         const SemNode*& source);
	void LowerStatementList(const vector<SemNodePtr>& items, size_t from);
	void LowerStatement(const SemNode& node);
	void LowerLocalDeclaration(const SemNode& node);
	void LowerLocalVariable(const SemNode& node);
	// PA20 function-local statics: the hoisted internal global, the
	// constant-init shortcut, and the first-use guard around the
	// declaration-point initialization.
	void LowerLocalStatic(const SemNode& node);
	void LowerLocalStaticInit(const SemNode& node, const string& base);
	// Emits the element stores of a braced local array into `slot`;
	// returns the emitted base-address temp.
	string LowerLocalArrayInit(const SemNode& node, const string& slot,
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
	// One spelled binary value operation; 128-bit division and
	// remainder expand through LowerWideDivMod (lower_expand.cpp).
	string EmitBinaryValue(const string& op_name, const TypePtr& type,
	                       const string& lhs, const string& rhs);
	string LowerWideDivMod(const string& op_name, const TypePtr& type,
	                       const string& lhs, const string& rhs);
	LowerValue LowerValueExpr(const SemNode& node);
	TypePtr CalleeFnType(const SemNode& callee);
	string LowerValueAs(const SemNode& node, const TypePtr& dest,
	                    ELowerConvertContext context);
	LowerValue ConvertValue(LowerValue value, const TypePtr& dest,
	                        ELowerConvertContext context);
	LowerValue ConvertToBool(LowerValue value, const TypePtr& source,
	                         const TypePtr& target);
	LowerValue ConvertPointerValue(LowerValue value,
	                               const TypePtr& source,
	                               const TypePtr& target);
	LowerValue ConvertIntegralImmediate(LowerValue value,
	                                    const TypePtr& source,
	                                    const TypePtr& target);
	string LowerAddressExpr(const SemNode& node);
	string CallResultAddress(const SemNode& node);
	string LowerPointerOperand(const SemNode& node);
	string LowerPointerCmpOperand(const SemNode& node);
	void LowerCondition(const SemNode& node, const string& true_label,
	                    const string& false_label);
	void BranchOnValue(const SemNode& node, const string& true_label,
	                   const string& false_label);
	void LowerEffect(const SemNode& node);
	// PA29 GNU statement expressions (lower_expand.cpp): the trailing
	// expression statement's node ("" value when absent) and the
	// value/effect lowering over the bound compound statement.
	const SemNode* StatementExpressionTail(const SemNode& node);
	LowerValue LowerStatementExpression(const SemNode& node,
	                                    bool as_value);
	string LowerStatementExpressionReference(const SemNode& node,
	                                         const TypePtr& referee);
	// `result_address` carries the caller-owned destination of an
	// indirect class-valued call ("" lets class results materialize at
	// the consumer).
	LowerValue LowerCall(const SemNode& node,
	                     const string& result_address = "");
	bool SkipTrivialDefaultConstruction(const SemNode& node,
	                                    const SemNode& callee);
	// PA29 float-classification builtins (lower_expand.cpp): a direct
	// call to the global-scope __builtin_isnan / __builtin_nanl
	// expands inline (an f80 storage-image bit test / a `const f80
	// nanL`), so the NaN query never lowers to a call to itself.
	bool IsFloatBuiltinCall(const SemNode& node) const;
	LowerValue LowerFloatBuiltin(const SemNode& node,
	                             const SemNode& callee);
	// PA33 vararg/stack builtins (lower_expand.cpp): va_end drops,
	// va_start/alloca lower to backend-expanded role calls, va_arg
	// expands to the SysV cursor walk.
	bool IsVarargBuiltinCall(const SemNode& node) const;
	LowerValue LowerVarargBuiltin(const SemNode& node,
	                              const SemNode& callee);
	LowerValue LowerVaArg(const SemNode& node);
	// --- call-argument binding (lower_arg_bind.cpp) ---
	string LowerCallArgument(const SemNode& node, const TypePtr& param);
	string LowerReferenceArgument(const SemNode& node,
	                              const TypePtr& referee);
	// --- PA16 class value transfers ---
	// Constructs/copies the class value of `node` into the object at
	// `dest` (constructor actions, class-valued calls, conditionals).
	void LowerClassInit(const SemNode& node, const string& dest);
	// PA24: closure construction (field-wise capture-pointer stores).
	void LowerClosureInit(const SemNode& node, const string& dest);
	// PA25 18.9: initializer_list record construction from its braced
	// elements (backing array + {begin, size} stores).
	void LowerInitializerListInit(const SemNode& node,
	                              const string& dest);
	// Materializes a class-valued call result in a fresh `kind` slot
	// (or into `dest` when given); returns the object address text.
	string MaterializeClassResult(const SemNode& call, const char* kind,
	                              const string& dest);
	// One by_address call argument object (lower_arg_bind.cpp).
	string MaterializeClassArg(const SemNode& node, const TypePtr& bare);
	// --- PA16 allocation expressions (lower_new.cpp) ---
	LowerValue LowerNewArray(const SemNode& node);
	LowerValue LowerNewInit(const SemNode& node);
	void LowerDelete(const SemNode& node);
	LowerValue LowerComparison(const SemNode& node);
	LowerValue LowerBinary(const SemNode& node);
	LowerValue LowerLogicalValue(const SemNode& node);
	LowerValue LowerUnary(const SemNode& node);
	LowerValue LowerIncDec(const SemNode& node, bool prefix);
	LowerValue LowerAssignment(const SemNode& node);
	LowerValue LowerCompoundAssignment(const SemNode& node,
	                                   const LowerValue& target,
	                                   const SemNode* member_lhs = 0);
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
	// A derived-to-base address adjustment: one base-subobject
	// projection regardless of derivation depth, carrying the unique
	// derivation path's total byte offset (PA26). The `from`/`to` form
	// resolves the path from the typed class records; the hops form
	// renders a sema-resolved adjustment.
	string AdjustToBase(const string& address, const NamedTypeInfo* from,
	                    const NamedTypeInfo* to);
	string AdjustToBaseHops(const string& address, int hops,
	                        unsigned long long offset);
	// PA27: the address behind a virtual-edge member path (carrier
	// virtual base resolved per context, then the hops inside it).
	string VBaseCarrierAddress(const SemNode& node, bool with_projection);
	string VBaseSubobjectAddress(const SemNode& object, size_t vbase_index,
	                             unsigned long long remainder,
	                             const NamedTypeInfo* owner,
	                             bool with_projection);
	string VBaseRemainderHops(const string& base, const ClassInfo& carrier,
	                          const NamedTypeInfo* owner,
	                          unsigned long long remainder,
	                          bool with_projection);
	// A runtime class-pointer adjustment across a displaced base:
	// branches so null stays null (4.10p3 / 5.2.9p11); the PA27 shape
	// stores 0 / the shifted address into one $basecast slot.
	string AdjustPointerGuarded(const string& value, long long delta);
	// PA26 member pointers (lower_member_pointer.cpp): `&C::member`
	// constants, `.*` / `->*` data-member access, and the bound
	// member-pointer call pieces.
	LowerValue LowerMemberPointerConstant(const SemNode& node);
	string MemberPointerAccessAddress(const SemNode& node);
	LowerValue LowerMemberPointerValue(const SemNode& node);
	// May shift `object_text` by the value's this-adjustment (a
	// displaced-base class's runtime member pointer).
	string LowerMemberPointerCallee(const SemNode& callee,
	                                string& object_text);
	TypePtr MemberPointerCallSignature(const TypePtr& fn_type);
	// Registers a by-value class parameter's attached destructor
	// actions as function-scope cleanups (the callee owns them).
	void RegisterParameterCleanup(const SemNode& parameter);
	// PA16 allocation expressions (lower_new.cpp).
	LowerValue LowerNewConstruction(const SemNode& node);
	void LowerNewArrayCtorLoop(const SemNode& ctor, const string& data,
	                           unsigned long long elem_size,
	                           const string& bound);
	void LowerNewArrayZeroLoop(const string& result, const string& bound);
	LowerValue LowerMemberValue(const SemNode& node);
	LowerValue LowerBitFieldValue(const SemNode& node);
	LowerValue LowerBitFieldAssignment(const SemNode& node);
	LowerValue LowerMemberAssignment(const SemNode& node);
	void LowerClassLocal(const SemNode& node);
	bool LowerNrvoLocal(const SemNode& node);
	// True when an empty-class local's operator-call initializer
	// drops (the pinned reference shape); demands the callee.
	bool ElideEmptyOperatorInit(const SemNode& node,
	                            const SemNode& child);
	void LowerConstructorCall(const SemNode& action,
	                          const string& this_text);
	// PA16 (lower_transfer.cpp): a trivial copy/move construction
	// lowers as a raw object copy (`copyobj`) instead of a synthesized
	// helper call. The destination is `this_text` when non-empty, else
	// the action's own address child.
	// PA33 -O1 (lower_transfer.cpp): call-site expansion of a simple
	// inline constructor (literal member stores only).
	bool LowerSimpleInlineConstruction(const SemNode& action,
	                                   const SemNode& callee,
	                                   const string& this_text);
	void LowerTrivialCopyAction(const SemNode& action,
	                            const string& this_text);
	// A class temporary: a fresh object slot plus its constructor run;
	// returns the address temp ("%tN"). `register_cleanup` is false
	// for by-value argument objects, which the callee destroys.
	string MaterializeTemporary(const SemNode& action, const char* kind,
	                            bool register_cleanup);
	string ClassArrayElement(const string& base, const LowerValue& index,
	                         const TypePtr& element);

	// --- PA17 polymorphic lowering (lower_member.cpp) ---
	// SN_VPOINTER_STORE: stores the class's vtable entry pointer into
	// the object addressed by children[0].
	void LowerVPointerStore(const SemNode& node);
	// The trailing `operator delete(this)` call of a deleting
	// destructor entry (special_code "D0").
	void EmitDeletingEpilogue();
	// Scalar delete of a class with a virtual destructor: the indirect
	// call through the deleting-destructor slot.
	void LowerDeletingDispatch(const SemNode& dtor_callee,
	                           const string& pointer_text);
	// The `as (...) -> ...` suffix of an indirect (or dispatched) call.
	// `method_object` marks a leading this parameter (dispatch), which
	// never carries its own hidden vbase pointers.
	string IndirectCallSignature(const TypePtr& fn_type,
	                             bool method_object = false);

	// --- PA15 lifetime (lower_member.cpp) ---
	// Full-expression temporaries (12.2): registered as each one is
	// constructed, destroyed at the end of the enclosing full
	// expression and from every unwind dispatch while live.
	struct TempCleanup
	{
		TempCleanup() : action(0) {}

		string address;         // the materialized slot's address temp
		const SemNode* action;  // SN_DESTRUCTOR_ACTION (no address)
	};
	void LowerConstructorAction(const SemNode& node);
	// The construction call of one statement-position action (the
	// region/arming wrapper above owns the 15.2p2 bookkeeping).
	void LowerConstructorActionCall(const SemNode& node);
	// PA29 15.2p2: destroys the constructor's armed fully-constructed
	// subobjects (reverse construction order) on paths where the
	// exception leaves the function.
	void EmitCtorUnwindCleanups();
	void BeginFullExpression(const SemNode& root,
	                         bool open_segment = true);
	void EndFullExpression();
	bool ScanArmsCleanups(const SemNode& node, bool& live) const;
	void EmitTempCleanups(size_t from);
	void OpenEhRegion(const char* end_prefix = "call_unwind_end");
	// PA25 5.2.8: a call node that is std::type_info::operator== or
	// operator!= over typeid results (folds to RTTI address identity;
	// the class identity is the sema-recognized std::type_info).
	bool IsTypeInfoComparison(const SemNode& node) const;
	// --- PA25 source-level exception handling ---
	// One active try or catch context. A try context carries its
	// dispatch/entry/end labels and handler table; a catch context its
	// cleanup label (normal exits run __cxa_end_catch).
	struct EhHandler
	{
		EhHandler() : catch_all(false), selector(0), node(0) {}

		string rtti;    // "@..." record of the handler type ("" for ...)
		bool catch_all;
		int selector;   // function-global 1-based id (0: unassigned)
		const SemNode* node;
		string body_label;
	};
	struct EhContext
	{
		EhContext() : is_catch(false), ctor_rethrow(false),
		              cleanup_depth(0) {}

		bool is_catch;
		// PA29 15.3p15: a constructor function-try-block handler
		// implicitly rethrows when it flows off its end.
		bool ctor_rethrow;
		string dispatch_label;
		string entry_label;
		string next_label;
		string end_label;
		string cleanup_label;  // catch contexts: the unwind path
		vector<EhHandler> handlers;
		size_t cleanup_depth;  // cleanup_scopes_.size() at the try
	};
	// Emits the try's handler markers (eh_catch/eh_catch_all lines),
	// assigning function-global selectors on first emission.
	void EmitTryMarkers(EhContext& context);
	void EmitCatchHandler(EhContext& context, EhHandler& handler,
	                      const string& exc);
	void EmitCatchNext(EhContext& context);
	// The eh_end / __cxa_end_catch pops a return crossing the active
	// contexts must run, interleaved with the scope cleanups.
	void EmitEhReturnUnwind();
	void LowerTry(const SemNode& node);
	void LowerThrow(const SemNode& node);
	// 15.3p16-p17: binds the handler's exception-declaration from the
	// caught object; fills the by-value parameter slot/dtor pair.
	void EmitCatchParamInit(const SemNode& handler_node,
	                        const string& object, string& param_slot,
	                        string& param_dtor);
	void EmitCatchParamDtor(const string& slot, const string& dtor);
	// PA25 5.2.7 dynamic_cast: null-checks the operand, calls the
	// __dynamic_cast runtime, and (reference form) raises
	// __cxa_bad_cast on failure; returns the result pointer text.
	string LowerDynamicCast(const SemNode& node);
	LowerValue LowerTypeInfoComparison(const SemNode& node,
	                                   const SemNode& callee);
	string LowerCalleeText(const SemNode& callee, const TypePtr& fn_type,
	                       bool direct, bool dispatch,
	                       string& object_text);
	// PA25 typeid: the address of the RTTI record the query denotes
	// (static: the operand type's record; dynamic: read through the
	// polymorphic operand's vpointer with a null-check).
	string LowerTypeidAddress(const SemNode& node);
	// A zero-valued return matching the function's return type (the
	// dead terminator after a noreturn runtime call).
	void EmitZeroValueReturn();
	// Opens the dispatch region at the start of a straight-line
	// evaluation segment (a full expression or a conditionally
	// evaluated arm) when the segment runs a call under pending
	// cleanups or armed temporaries.
	void OpenSegmentRegion(const SemNode& root);
	bool SegmentContainsCall(const SemNode& node, bool may_unwind_only);
	// The rendered content identity of the current unwind-dispatch
	// cleanups; regions with equal signatures share one dispatch
	// block.
	string CleanupSignature() const;
	void PushCleanupScope();
	void PopCleanupScope(bool emit);
	void RegisterCleanup(const vector<const SemNode*>& actions);
	// Emits the destructor actions of the scopes at depth >= `from`,
	// innermost scope first; later objects destroy before earlier ones,
	// array elements in their recorded order.
	void EmitCleanupsFrom(size_t from);
	// The destructor's subobject destructions (12.4): the trailing
	// function-node actions, or the pre-handler try-region actions of
	// a destructor function-try-block. Returns leave through them.
	void CollectDtorEpilogue(size_t first_statement);
	void LowerDestructorActionStatement(const SemNode& node);
	void EmitDtorEpilogueActions();
	bool HaveCleanups() const;
	// Scope cleanups registered above the innermost try/catch context's
	// depth: no landing edge runs them, so a raise over them needs its
	// own dispatch region.
	bool HaveCleanupsAboveEhBoundary() const;
	// True when a synthetic route into an enclosing try's entry chain
	// must leave the outer try's frame record armed (two or more
	// same-frame enclosing tries; the outer markers never ran here, so
	// the resume re-landing owns the outer try).
	bool RouteKeepsOuterTryArmed() const;
	void CloseEhRegion();

	LowerProgram& program_;
	const SemNode& def_;
	const LowFunctionInfo& info_;
	TypePtr return_type_;

	vector<Block> blocks_;
	set<string> referenced_;
	vector<ParamInfo> params_;
	set<string> param_names_;
	// PA27: the signature's hidden trailing pointers and the per-
	// parameter carried-entry maps ("this" holds the __vbptr row).
	vector<HiddenParam> hidden_params_;
	map<string, VBaseParamMap> vbase_params_;
	string vtt_param_;  // "%__vtt" when the entry carries one
	size_t declared_param_count_ = 0;
	// One call's hidden-argument supply shares each argument's anchor
	// (a pointer variable loads once for its whole row).
	map<const SemNode*, string> supply_anchors_;
	// PA16: lvalues addressed through a parameter or the indirect
	// result pointer instead of a local slot (by_address parameters and
	// the return-slot-reused local), keyed like slot_map_.
	map<pair<const void*, string>, string> address_aliases_;
	bool indirect_ret_;
	// The eligible top-level named local lowered directly in %ret.
	const Scope* nrvo_scope_;
	string nrvo_name_;
	bool nrvo_active_;
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
	// 6.6/15.1: each label's cleanup-scope and try/catch-context
	// depths relative to the function-body baseline, pre-scanned so a
	// goto destroys the automatic objects and closes the EH regions
	// it jumps out of.
	struct LabelContext
	{
		size_t cleanup_depth = 0;
		size_t eh_depth = 0;
	};
	map<string, LabelContext> label_contexts_;
	size_t goto_cleanup_base_ = 0;
	size_t goto_eh_base_ = 0;
	void ScanLabelContexts(const SemNode& node, size_t cleanups,
	                       size_t ehs);

	// --- PA15 lifetime state ---
	// Cleanup scopes parallel the lowered compound statements; each
	// scope holds per-object action groups in declaration order.
	vector<vector<vector<const SemNode*>>> cleanup_scopes_;
	// PA29 15.2p2: destructor actions of the constructor's fully-
	// constructed subobjects, in construction order. Unwind dispatches
	// that leave the function destroy them (in reverse) after the
	// temporaries and scope cleanups; normal exits never run them.
	vector<const SemNode*> ctor_cleanups_;
	// The destructor's subobject destruction actions in destruction
	// order (see CollectDtorEpilogue); entries before
	// dtor_epilogue_done_ already ran at statement position on the
	// fall-through path.
	vector<const SemNode*> dtor_epilogue_;
	size_t dtor_epilogue_done_;
	// Inside a ctor/dtor function-try-block handler: the unwind edge
	// already destroyed the subobjects, so returns skip the epilogue.
	bool in_function_try_handler_;
	// Parameter cleanup groups registered at entry (non-arming).
	size_t param_cleanup_count_;
	// The shared materialized return-object slot (one per function).
	string retobj_slot_;
	vector<size_t> break_cleanup_;
	vector<size_t> continue_cleanup_;
	// Destructor actions of the local just lowered, registered only
	// after its initializing full expression closes (the object is not
	// destroyed on unwind out of its own construction).
	vector<const SemNode*> pending_cleanup_;
	// The open unwind-dispatch region of the current full-expression.
	bool eh_open_;
	string eh_dispatch_;
	string eh_end_;
	// The open region reuses an already-emitted dispatch block (its
	// close prints only `eh_end` and stays in the current block).
	bool eh_reused_;
	string eh_pending_signature_;
	// Emitted dispatch blocks by cleanup-content signature.
	map<string, string> eh_dispatch_cache_;
	// Conditionally evaluated value arms drop their temporaries'
	// cleanups (the reference leaves conditional-arm temporaries
	// undestroyed rather than tracking them across the join).
	int cond_arm_depth_;
	// PA25: the active try/catch contexts and the function-global
	// catch-handler selector counter.
	vector<EhContext> eh_contexts_;
	int catch_selector_counter_;
	// PA33: the noexcept whole-body terminate dispatch label ("" when
	// this body arms none).
	string terminate_dispatch_;
	// The unwind leaves this frame: routes to the terminate dispatch
	// under the armed noexcept region (the host unwinder would re-land
	// the same pad forever on a bare resume), else resumes.
	void EmitUnwindLeave();
	// Class return-value construction into the result object never
	// opens unwind regions (the reference's final-action form).
	bool suppress_eh_regions_;
	// Lifetime-machinery calls (constructor actions, cleanups) never
	// open unwind regions.
	bool in_lifetime_action_;
	// --- full-expression temporary state ---
	vector<TempCleanup> temp_cleanups_;
	vector<size_t> fe_marks_;
	vector<char> fe_armed_;
	// The open full expression runs a call while a caller-owned
	// destructible temporary is live: its may-unwind calls run under
	// dispatch regions (including calls before the first cleanup
	// registers).
	bool eh_armed_;
	bool in_cleanup_emission_;
	int ctor_depth_;  // open LowerConstructorCall frames
};
