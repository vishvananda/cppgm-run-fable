#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

using std::map;
using std::pair;
using std::shared_ptr;
using std::string;
using std::vector;

#include "sema/class_info.h"
#include "sema/scope.h"
#include "sema/sem_node.h"
#include "sema/type.h"

// PA20 constant evaluation over analyzed semantic trees. The
// expression analyzer has already resolved names, overloads,
// conversions, and member layout into SemNodes; this engine only
// evaluates: typed constant objects (scalars, enums, pointers,
// references, arrays, class objects), constexpr function and
// constructor calls with locals, assignment, and loops, and the
// constant-object store that later lookups, template arguments, and
// the lowering read.

struct ConstObject;
typedef shared_ptr<ConstObject> ConstObjectPtr;

// A compile-time pointer or reference value: into an engine-owned
// object, or the symbolic address of a runtime global (address
// constant). All fields empty/zero means the null pointer.
struct ConstPointer
{
	ConstPointer() : sym_scope(0), offset(0) {}

	ConstObjectPtr object;
	const Scope* sym_scope;  // symbolic &global (no evaluated image)
	string sym_name;
	unsigned long long offset;  // byte offset within the target

	bool IsNull() const
	{
		return !object && !sym_scope;
	}
};

// One evaluated object: a typed byte image plus the pointer values
// stored inside it (pointer slots keep their bytes zero; the value
// travels symbolically in `ptr_slots`).
struct ConstObject
{
	ConstObject() : literal_node(0) {}

	TypePtr type;
	vector<unsigned char> bytes;
	map<unsigned long long, ConstPointer> ptr_slots;
	// The SN_LITERAL this object evaluates (string literals): image
	// slots addressing it render as references to the emitted literal.
	const SemNode* literal_node;
};

// One evaluated rvalue.
struct EvalValue
{
	enum EKind
	{
		EV_INT,   // integral / enum (value in ival, sign-extended)
		EV_FLOAT, // floating (host long double payload)
		EV_PTR    // pointer / reference target
	};

	EvalValue() : kind(EV_INT), fval(0) {}

	EKind kind;
	ConstValue ival;
	long double fval;
	ConstPointer ptr;
	TypePtr type;  // the C++ type of the value
};

// Indexes the unit's SN_FUNCTION_DEFINITION nodes so a resolved
// callee finds its analyzed body: specialization identity first, then
// the canonical qualified name plus signature equality (the same
// scheme the lowering keys emission on).
class ConstBodyRegistry
{
public:
	ConstBodyRegistry(SemUnit& unit);

	// The definition for a resolved SN_CALLEE, or null.
	const SemNode* Find(const SemNode& callee);

private:
	void Refresh();
	void Index(const SemNode& node);

	SemUnit& unit_;
	size_t items_seen_;
	size_t deferred_seen_;
	size_t synthesized_seen_;
	map<const void*, const SemNode*> by_spec_;
	// Primary identity: declaring scope + declared name (methods and
	// plain functions); the display-name map covers definitions whose
	// scope differs from the callee's (synthesized helpers).
	map<pair<const void*, string>, vector<const SemNode*>> by_entity_;
	map<string, vector<const SemNode*>> by_name_;
};

class ConstEvalEngine
{
public:
	explicit ConstEvalEngine(SemUnit& unit);

	// --- entry points (all throw runtime_error when the operand is
	// not a constant expression of the evaluated subset) ---

	// A constant condition contextually converted to bool (7p4).
	bool EvaluateCondition(const SemNode& expr);
	// An integral or enumeration constant (5.19 template/bound uses).
	ConstValue EvaluateIntegral(const SemNode& expr);
	// Any scalar constant (integral, floating, pointer).
	EvalValue EvaluateScalar(const SemNode& expr);
	// Full-object evaluation of an SN_VARIABLE item's analyzed
	// initializer children against the declared object type. The
	// object registers in the store under (scope, name) before the
	// children evaluate, so self-addressing constructor actions
	// resolve; a failed evaluation removes the entry.
	ConstObjectPtr EvaluateVariableInit(const SemNode& item,
	                                    const TypePtr& type,
	                                    const Scope* scope,
	                                    const string& name);

	// --- the constant-object store (evaluated named objects) ---
	void StoreObject(const Scope* scope, const string& name,
	                 const ConstObjectPtr& object);
	ConstObjectPtr FindObject(const Scope* scope,
	                          const string& name) const;
	// A one-scalar image (float/pointer constants readable by name).
	void StoreScalarObject(const Scope* scope, const string& name,
	                       const TypePtr& type, const EvalValue& value);

	// Records a folded integral constant on `node` so the
	// global-initializer lowering emits it instead of dynamic init.
	static void StampScalar(SemNode& node, const ConstValue& value);

	// --- frames (const_eval_stmt.cpp; public for the file-local
	// frame-stack RAII helper) ---
	struct Frame
	{
		Frame() : returned(false), ret_target_valid(false) {}

		map<pair<const void*, string>, ConstPointer> slots;
		bool returned;
		TypePtr ret_type;
		EvalValue ret_value;
		ConstPointer ret_target;  // class-valued results build here
		bool ret_target_valid;
	};

private:
	enum EFlow
	{
		FLOW_NORMAL,
		FLOW_RETURN,
		FLOW_BREAK,
		FLOW_CONTINUE
	};

	// --- evaluation core (const_eval_expr.cpp) ---
	// EvaluateCondition without the entry-point counter reset.
	bool EvaluateBool(const SemNode& expr);
	EvalValue EvalRead(const SemNode& node);
	ConstPointer EvalLValue(const SemNode& node);
	// Initializes the object at `dest` (declared type `type`) from an
	// analyzed initializer node.
	void EvalInit(const SemNode& node, const ConstPointer& dest,
	              const TypePtr& type);
	// One SN_VARIABLE initializer child: member-wise aggregate store
	// actions execute as effects; value forms initialize `dest`.
	void ApplyInitAction(const SemNode& child, const ConstPointer& dest,
	                     const TypePtr& type);
	void EvalEffect(const SemNode& node);
	EvalValue EvalId(const SemNode& node);
	ConstPointer IdAddress(const SemNode& node);
	EvalValue EvalLiteral(const SemNode& node);
	EvalValue EvalUnary(const SemNode& node);
	EvalValue EvalBinary(const SemNode& node);
	EvalValue EvalPointerBinary(const SemNode& node, EvalValue lhs,
	                            EvalValue rhs);
	EvalValue EvalAssignment(const SemNode& node);
	EvalValue EvalIncDec(const SemNode& node, bool prefix);
	EvalValue EvalConditional(const SemNode& node);
	EvalValue EvalSubscript(const SemNode& node);
	EvalValue EvalMember(const SemNode& node);
	ConstPointer MemberAddress(const SemNode& node);
	ConstPointer SubscriptAddress(const SemNode& node);
	EvalValue EvalCast(const SemNode& node);
	EvalValue EvalCallValue(const SemNode& node);
	EvalValue ReadThrough(const ConstPointer& address,
	                      const TypePtr& type);
	ConstObjectPtr StringObject(const SemNode& node);

	// --- calls and statements (const_eval_stmt.cpp) ---
	// Runs a resolved call; `result_dest` receives class-valued
	// results (allocated by the caller when needed).
	EvalValue EvalCall(const SemNode& node,
	                   const ConstPointer* result_dest);
	void BindArguments(const SemNode& call, const SemNode& definition,
	                   Frame& frame);
	void BindOneArgument(const SemNode& arg, const SemNode& param,
	                     Frame& frame);
	void RunBody(const SemNode& definition, Frame& frame);
	EFlow ExecStatement(const SemNode& node, Frame& frame);
	EFlow ExecCompound(const SemNode& node, Frame& frame);
	EFlow ExecIf(const SemNode& node, Frame& frame);
	EFlow ExecLoop(const SemNode& node, Frame& frame);
	EFlow ExecFor(const SemNode& node, Frame& frame);
	EFlow ExecReturn(const SemNode& node, Frame& frame);
	void ExecLocalVariable(const SemNode& node, Frame& frame);
	bool EvalConditionNode(const SemNode& node, Frame& frame);
	void EvalConstructorAction(const SemNode& node,
	                           const ConstPointer* dest,
	                           const TypePtr& dest_type);
	void RunConstructor(const SemNode& call, const SemNode& definition,
	                    const ConstPointer& object, size_t args_at);
	void EvalBracedInit(const SemNode& node, const ConstPointer& dest,
	                    const TypePtr& type);

	// --- typed storage access (const_eval.cpp) ---
	ConstObjectPtr Allocate(const TypePtr& type);
	EvalValue ReadScalar(const ConstObject& object,
	                     unsigned long long offset, const TypePtr& type);
	void WriteScalar(ConstObject& object, unsigned long long offset,
	                 const TypePtr& type, const EvalValue& value);
	void WriteValue(const ConstPointer& dest, const TypePtr& type,
	                const EvalValue& value);
	EvalValue Convert(const EvalValue& value, const TypePtr& dest);
	void CopyBytes(const ConstPointer& dest, const ConstPointer& src,
	               unsigned long long size);
	void Step();
	ConstObject& Writable(const ConstPointer& address);
	const ConstObject& Readable(const ConstPointer& address) const;

	// The innermost frame (empty stack outside calls: namespace-scope
	// evaluation reads only the store and folded facts).
	Frame* CurrentFrame();

	SemUnit& unit_;
	ConstBodyRegistry registry_;
	map<pair<const void*, string>, ConstObjectPtr> store_;
	map<const SemNode*, ConstObjectPtr> string_objects_;
	vector<Frame*> frames_;
	int depth_;
	long long steps_;
};

// Whether the analyzed tree contains a potentially-evaluated call to
// a callee without a non-throwing specification (5.3.7 noexcept over
// resolved facts; also the unwind-fact walk of synthesized bodies).
bool SemTreeMayThrow(const SemNode& node);

// The host long double value of a floating pp-number spelling (false
// when the spelling is not a floating literal).
bool DecodeFloatLiteral(const string& spelling, long double& out);

// The canonical unsuffixed decimal of a floating constant: the
// shortest form that round-trips at the destination type's precision
// (the reference presentation of scalar float global initializers).
string RenderFloatConstant(long double value, EFundamentalType type);
