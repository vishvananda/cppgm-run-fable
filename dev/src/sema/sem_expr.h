#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "ast/ast.h"
#include "sema/class_info.h"
#include "sema/scope.h"
#include "sema/sem_convert.h"
#include "sema/sem_node.h"

// PA12 expression analysis: resolves a PA10 expression tree against the
// PA11 scope/type model, producing the dump node plus the (type, value
// category) facts the enclosing context composes with. The analyzer
// reaches the binder through ISemExprHost - name and type resolution
// stay in the binder, the clause 4/5/13 rules stay here.

struct SemValue;

// The analyzer's view of the PA12 binder.
struct ISemExprHost
{
	virtual Scope* CurrentScope() = 0;
	virtual TypesModel& Model() = 0;
	// Resolution of a possibly qualified value name; `member_class` is
	// set when the binding is a class member (null otherwise). Throws
	// when the name does not resolve.
	virtual const ScopeBinding* ResolveValue(
		const AstName& name, const NamedTypeInfo*& member_class) = 0;
	// The type a callee name denotes, or null when it does not name a
	// type (functional-cast disambiguation).
	virtual TypePtr TryResolveCalleeType(const AstName& name) = 0;
	// 8.4 type-id resolution (casts, sizeof).
	virtual TypePtr ResolveCastTypeId(const AstTypeId& type_id) = 0;
	// Constant evaluation attempt for the __builtin_constant_p fold.
	virtual bool TryEvaluateConstant(const AstExpr& expr,
	                                 ConstValue& value) = 0;
	// PA15: the lazily declared __builtin_* functions (null when the
	// name is not a known builtin).
	virtual const ScopeBinding* ResolveBuiltinFunction(
		const string& name) = 0;
	// --- PA15 object-model context ---
	virtual ClassRegistry& Classes() = 0;
	// The class whose member body is being analyzed (null otherwise).
	virtual const ClassInfo* CurrentClass() = 0;
	// The `this` type inside a non-static member body (null otherwise).
	virtual TypePtr CurrentThisType() = 0;
	// Throws unless a member with the given declared access, found in
	// the class scope `owner`, is accessible from the current context
	// (clause 11 with friendship). `naming` is the class of the object
	// expression when known; protected access through friendship of an
	// intermediate derived class (11.2p5/11.4) needs it.
	virtual void CheckMemberAccess(const Scope* owner, EMemberAccess access,
	                               const string& what,
	                               const NamedTypeInfo* naming = 0) = 0;
	// 11.2p4: whether the current context is `cls` itself, one of its
	// members, or one of its friends (the naming-class rule grants such
	// contexts full access along the inheritance chain).
	virtual bool InClassContextOrFriend(const NamedTypeInfo* cls) = 0;
	// A constructor-call action over `cls` (ctor_index -1 selects the
	// synthesized implicit default constructor).
	virtual SemNodePtr MakeConstructorCall(const ClassInfo& cls,
	                                       int ctor_index, bool base_entry,
	                                       SemNodePtr address,
	                                       vector<SemNodePtr> args) = 0;
	// A temporary's destructor action (no address subtree); demanded
	// for full-expression temporaries of destructible classes.
	virtual SemNodePtr MakeTemporaryDtor(const ClassInfo& cls) = 0;
	// --- PA16 value semantics ---
	// Constructor overload resolution over the class's constructors
	// (user-declared plus the implicitly declared copy/move members);
	// applies conversions and default arguments, returns -1 for the
	// implicit default constructor.
	virtual int ResolveClassCtorHost(const ClassInfo& cls,
	                                 vector<SemValue>& args, bool copy_init,
	                                 const char* what) = 0;
	// Demand-synthesis of an implicitly declared copy/move assignment
	// operator selected by overload resolution.
	virtual void EnsureAssignSpecial(const NamedTypeInfo* cls_entity,
	                                 size_t overload_index) = 0;
	// A braced aggregate temporary: the synthesized field-wise
	// constructor over the converted items (8.5.1 over a prvalue).
	virtual SemNodePtr MakeAggregateTemporary(const ClassInfo& cls,
	                                          vector<SemValue> args) = 0;
	virtual ~ISemExprHost() {}
};

// One analyzed expression: the dump node plus composition facts. The
// node's printed type may keep a reference (call results, casts to
// reference types); `type` is always the reference-stripped type the
// enclosing expression computes with.
struct SemValue
{
	SemValue()
		: category(VC_PRVALUE), null_pointer_literal(false),
		  function_set(false), fn_owner(0), member_class(0), member_fn(0)
	{}

	SemNodePtr node;
	TypePtr type;
	EValueCategory category;
	bool null_pointer_literal;  // literal of integer type, value 0
	// An id-expression naming one or more functions (resolution may be
	// target-directed, 13.4).
	bool function_set;
	vector<TypePtr> overloads;
	const Scope* fn_owner;  // declaring scope (canonical callee name)
	string fn_name;
	// Set when the id names a class member: the class entity and the
	// declared member type (member-function types keep their trailing
	// cv here; the node shows the this-adjusted spelling).
	const NamedTypeInfo* member_class;
	TypePtr member_type;
	// PA15 bound member function set: the method's binding plus the
	// analyzed object expression (null for static/unbound uses).
	const ScopeBinding* member_fn;
	SemNodePtr member_object;
};

// One user-declared operator candidate (sem_operator.cpp).
struct OperatorCandidate
{
	OperatorCandidate() : binding(0), index(0), is_member(false) {}

	const ScopeBinding* binding;
	size_t index;     // overload position in the binding
	bool is_member;
	TypePtr declared; // declared function type
};

// The canonical qualified name of a binding declared in `owner`: the
// named namespace path from the global scope (unnamed namespace
// components are skipped) joined with `::`.
string CanonicalQualifiedName(const Scope* owner, const string& name);

class SemExprAnalyzer
{
public:
	explicit SemExprAnalyzer(ISemExprHost& host);

	SemValue Analyze(const AstExpr& expr);

	// 8.5 copy-initialization of a `dest`-typed object, parameter, or
	// return value (references bind): validates the conversion, retypes
	// null pointer literals, and resolves target-directed function
	// sets. Throws when no conversion exists.
	void CopyInitialize(SemValue& value, const TypePtr& dest,
	                    const char* what);

	// Braced initialization of the supported array forms; returns the
	// braced-init-list node (completing unknown bounds via `dest`).
	SemNodePtr AnalyzeBracedInit(const AstExpr& braced, TypePtr& dest);

	// PA16: contextual bool conversion; a class operand materializes
	// its conversion-function call into the value.
	void RequireContextualBool(SemValue& value, const char* what);
	// A class operand of a built-in operator form converts through its
	// single non-explicit conversion function (13.6 subset).
	bool ConvertClassOperand(SemValue& value);

	// PA15: applies a classified conversion's dump effects (overload
	// selection, null-pointer retyping) for callers that ranked the
	// conversion themselves (constructor selection, operator calls).
	void ApplyConversion(SemValue& value, const ImplicitConversion& conv,
	                     const TypePtr& dest);
	// The converting-constructor arm of ApplyConversion (12.3.1).
	void ApplyConstructorConversion(SemValue& value,
	                                const ImplicitConversion& conv);

	// PA16: 13.3.1.2 selection over the user-declared (and implicitly
	// declared assignment) operator candidates; public so the binder's
	// special-member synthesis reuses it for member-wise assignment.
	bool ResolveOperatorCall(const string& spelling,
	                         vector<SemValue>& operands, bool member_only,
	                         SemValue& result);
	TypePtr CandidateSignature(const OperatorCandidate& candidate,
	                           const SemValue& object);
	size_t AppendBuiltinCandidate(const string& spelling,
	                              const vector<SemValue>& operands,
	                              bool member_only,
	                              vector<TypePtr>& ranking,
	                              vector<size_t>& viable_arity);

private:
	SemValue AnalyzeLiteral(const AstExpr& expr);
	SemValue AnalyzeKeywordLiteral(const AstExpr& expr);
	SemValue AnalyzeId(const AstExpr& expr);
	SemValue AnalyzeCall(const AstExpr& expr);
	SemValue AnalyzeNamedCall(const AstExpr& expr,
	                          const ScopeBinding& binding,
	                          const NamedTypeInfo* member_class);
	SemValue AnalyzeIndirectCall(const AstExpr& expr);
	// PA16: an unqualified call with argument-dependent lookup
	// (sem_operator.cpp); `visible` is the ordinary-lookup binding.
	SemValue AnalyzeAdlCall(const AstExpr& expr, const string& name,
	                        const ScopeBinding* visible);
	SemValue AnalyzeBuiltinConstantP(const AstExpr& expr);
	SemValue AnalyzeUnary(const AstExpr& expr);
	SemValue AnalyzeAddressOf(const AstExpr& expr);
	SemValue AnalyzeIncDec(const AstExpr& expr, bool prefix);
	SemValue AnalyzeBinary(const AstExpr& expr);
	SemValue AnalyzeAdditive(const AstExpr& expr, SemValue& lhs,
	                         SemValue& rhs);
	SemValue AnalyzeComparison(const AstExpr& expr, SemValue& lhs,
	                           SemValue& rhs);
	SemValue AnalyzeAssignment(const AstExpr& expr);
	SemValue AnalyzeConditional(const AstExpr& expr);
	TypePtr ConditionalResultType(const SemValue& a, const SemValue& b,
	                              EValueCategory& category);
	SemValue AnalyzeSubscript(const AstExpr& expr);
	SemValue AnalyzeMember(const AstExpr& expr);
	SemValue AnalyzeCastTo(const TypePtr& dest, const AstExpr& operand,
	                       bool has_anno, ETokenType op,
	                       const string& op_spelling);
	SemValue AnalyzeFunctionalCast(const TypePtr& dest,
	                               const vector<AstExprPtr>& arguments);
	SemValue AnalyzeSizeof(const AstExpr& expr);
	SemValue CallResult(const TypePtr& function_type);
	// PA16: copy/move-initialization of a by-value class destination
	// (call arguments, declared objects, return values): resolves the
	// copy/move constructor and wraps the source in a synth_copy
	// constructor action.
	void WrapClassValueInit(SemValue& value, const TypePtr& bare);

	// --- PA15 member access and method calls (sem_member.cpp) ---
	SemValue AnalyzeMemberAccess(SemValue object, const string& name,
	                             ETokenType op, bool implicit_this);
	SemValue AnalyzeImplicitMember(const ScopeBinding& binding,
	                               const string& written);
	SemValue AnalyzeMemberCall(const AstExpr& expr,
	                           const AstExpr& callee);
	SemValue AnalyzeMethodCall(SemValue object, const ScopeBinding& binding,
	                           const vector<AstExprPtr>& arguments);
	SemValue AnalyzeStaticMethodCall(const AstExpr& expr,
	                                 const ScopeBinding& binding);
	SemValue AnalyzeStaticMemberValue(const ScopeBinding& binding,
	                                  const string& written);
	SemValue MakeTemporaryObject(const TypePtr& class_type,
	                             const vector<AstExprPtr>& arguments);
	// --- PA16 allocation expressions (sem_new.cpp) ---
	SemValue AnalyzeNew(const AstExpr& expr);
	SemValue AnalyzeNewArray(const AstExpr& expr, size_t bound_item);
	SemValue AnalyzeDelete(const AstExpr& expr);
	SemValue MakeAllocationCall(const char* name, vector<SemValue> args,
	                            const TypePtr& result_type,
	                            bool& unwind_no);
	SemValue MakeSizeLiteral(unsigned long long size);
	SemValue AnalyzeStringUdl(const AstExpr& expr);
	// --- PA15 operator overloading (sem_operator.cpp) ---
	void CollectOperatorCandidates(const string& op_name,
	                               const vector<SemValue>& operands,
	                               bool member_only,
	                               vector<OperatorCandidate>& out);
	static bool OperatorOperand(const SemValue& value);
	bool TryBinaryOperator(const string& spelling, SemValue& lhs,
	                       SemValue& rhs, SemValue& result);
	bool TryUnaryOperator(const string& spelling, SemValue& operand,
	                      bool postfix, SemValue& result);
	SemValue AnalyzeFunctorCall(SemValue object, const AstExpr& expr);
	static AstName MakeDestructorTypeName(const AstName& name);
	SemValue MakeExplicitDestructorCall(SemValue object,
	                                    const ClassInfo& cls, bool arrow);
	SemValue DereferenceObject(SemValue object);
	SemNodePtr ImplicitThisObject();
	SemNodePtr AddressOfObject(SemNodePtr object);
	SemValue MakeBinaryNode(const AstExpr& expr, SemValue& lhs,
	                        SemValue& rhs, EValueCategory category,
	                        const TypePtr& type);
	void CheckCallArguments(const TypePtr& function_type,
	                        vector<SemValue>& args);
	TypePtr CompositePointerType(const SemValue& a, const SemValue& b);
	void RequireModifiableLvalue(const SemValue& value, const char* what);
	TypePtr ThisAdjustedType(const NamedTypeInfo* cls,
	                         const TypePtr& member) const;

	ISemExprHost& host_;
};

// The conversion-relevant facts of an analyzed value.
ConversionSource MakeConversionSource(const SemValue& value);

