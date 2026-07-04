#pragma once

#include <set>
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
	// --- PA18 templates ---
	// Deduce `tmpl` against the call arguments and return the
	// instantiated specialization (null when deduction fails, 14.8.3:
	// the template contributes no candidate).
	// `explicit_part`, when given, is the call's template-id: its
	// arguments pre-bind the leading parameters (14.8.1).
	virtual const FunctionSpecialization* DeduceFunctionTemplate(
		TemplateInfo& tmpl, const vector<SemValue>& args,
		const AstNamePart* explicit_part) = 0;
	// Swap the analyzer's lookup scope (default-argument analysis of
	// instantiated signatures rebinds under the argument alias scope);
	// returns the previous scope.
	virtual Scope* SwapLookupScope(Scope* scope) = 0;
	// PA18 14.5.6.2 subset: partial-ordering tie-break between two
	// deduced template candidates of one viable call.
	virtual bool TemplateCandidateMoreSpecialized(
		const FunctionSpecialization* a,
		const FunctionSpecialization* b, size_t argc) = 0;
	// PA18: completes a deferred member-class definition when the
	// context requires the complete type (14.7.1p1).
	virtual void RequireCompleteType(const NamedTypeInfo* info) = 0;
	// PA18 13.4p2: deduce `tmpl` against a target function type (null
	// when deduction fails).
	virtual const FunctionSpecialization* DeduceFunctionTemplateFromTarget(
		TemplateInfo& tmpl, const TypePtr& target) = 0;
	// PA18 14.7.1p2: a resolved use selected this specialization - the
	// analyzer reports every winner so the host instantiates its body
	// (deduction alone composes only the signature).
	virtual void OnSpecializationOdrUsed(
		const FunctionSpecialization* spec) = 0;
	// Marks an unevaluated operand (3.2p2: no odr-use inside); returns
	// the previous state for restoring.
	virtual bool SwapUnevaluatedOperand(bool active) = 0;
	// PA19 14.7.1: a non-folding reference to a static data member of
	// a class-template specialization - the host instantiates the
	// member's registered out-of-class definition on this demand.
	virtual void OnStaticMemberReferenced(const ScopeBinding& binding)
	{
		(void)binding;
	}
	// PA19 5.3.3p5: sizeof...(name).
	virtual size_t PackSize(const string& name) = 0;
	// PA19 13.5.8: the numeric literal-operator template of `binding`
	// instantiated over the literal's source characters (null when no
	// char-pack template fits).
	virtual const FunctionSpecialization* InstantiateCharPackLiteral(
		const ScopeBinding& binding, const string& chars) = 0;
	// PA19 14.5.3: `pattern...` inside an argument or initializer
	// list; false when the pattern mentions no expandable pack.
	virtual bool ExpandPackExpression(const AstExpr& pattern,
	                                  vector<SemValue>& out) = 0;
	virtual ~ISemExprHost() {}
};

// Routes SelectBestOverload's template tie-break to the host's
// 14.5.6.2 partial-ordering subset over the call's deduced
// specializations (indexed like the candidate list; null entries are
// non-template candidates).
struct SpecOverloadOrder : OverloadOrder
{
	SpecOverloadOrder(ISemExprHost& host,
	                  const vector<const FunctionSpecialization*>& specs,
	                  size_t argc)
		: host_(host), specs_(specs), argc_(argc) {}
	virtual bool MoreSpecialized(size_t a, size_t b) const
	{
		const FunctionSpecialization* first =
			a < specs_.size() ? specs_[a] : 0;
		const FunctionSpecialization* second =
			b < specs_.size() ? specs_[b] : 0;
		return host_.TemplateCandidateMoreSpecialized(first, second,
		                                              argc_);
	}
private:
	ISemExprHost& host_;
	const vector<const FunctionSpecialization*>& specs_;
	size_t argc_;
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
	// PA18: the function templates declared under the name (target
	// -directed uses deduce against the destination type, 13.4p2) and,
	// per overload entry, the specialization it came from (null for
	// ordinary overloads).
	vector<TemplateInfo*> fn_templates;
	vector<const FunctionSpecialization*> overload_specs;
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
	OperatorCandidate() : binding(0), index(0), is_member(false),
	                      spec(0) {}

	const ScopeBinding* binding;
	size_t index;     // overload position in the binding
	bool is_member;
	TypePtr declared; // declared function type
	// PA18: set when the candidate is a deduced function-template
	// specialization (13.3.3 prefers non-templates on a tie).
	const FunctionSpecialization* spec;
};

// PA18: the scope default-argument expressions of `binding` analyze
// in — non-null only for bindings inside a template-argument alias
// scope, whose defaults reference the template parameters (8.3.6p9
// evaluation stays at the call site; lookup uses the declaration
// context).
Scope* TemplateDefaultArgScope(const ScopeBinding& binding);

// The canonical qualified name of a binding declared in `owner`: the
// named namespace path from the global scope (unnamed namespace
// components are skipped) joined with `::`.
string CanonicalQualifiedName(const Scope* owner, const string& name);

class SemExprAnalyzer
{
public:
	explicit SemExprAnalyzer(ISemExprHost& host);

	SemValue Analyze(const AstExpr& expr);
	// A constructed (or aggregate-initialized) temporary of a class
	// type from an argument/braced-init list (5.2.3, 8.5.4); the
	// binder's braced-return path builds through it too.
	SemValue MakeTemporaryObject(const TypePtr& class_type,
	                             const vector<AstExprPtr>& arguments,
	                             bool braced_assign);

	// 8.5 copy-initialization of a `dest`-typed object, parameter, or
	// return value (references bind): validates the conversion, retypes
	// null pointer literals, and resolves target-directed function
	// sets. Throws when no conversion exists.
	void CopyInitialize(SemValue& value, const TypePtr& dest,
	                    const char* what);

	// Braced initialization of the supported array forms; returns the
	// braced-init-list node (completing unknown bounds via `dest`).
	SemNodePtr AnalyzeBracedInit(const AstExpr& braced, TypePtr& dest);
	// One argument/initializer list with `pattern...` items expanded
	// in place (PA19 14.5.3).
	void AnalyzeArgumentList(const vector<AstExprPtr>& items,
	                         vector<SemValue>& out);

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
	                          const NamedTypeInfo* member_class,
	                          bool qualified);
	SemValue AnalyzeIndirectCall(const AstExpr& expr);
	// PA16: an unqualified call with argument-dependent lookup
	// (sem_operator.cpp); `visible` holds the ordinary-lookup function
	// bindings (several when same-level using-directive imports merge
	// into one overload set, 7.3.4p6).
	SemValue AnalyzeAdlCall(const AstExpr& expr, const string& name,
	                        const vector<const ScopeBinding*>& visible);
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
	SemValue AnalyzeCastToReference(const TypePtr& dest, SemValue value,
	                                ETokenType op);
	SemValue AnalyzeCastTo(const TypePtr& dest, const AstExpr& operand,
	                       bool has_anno, ETokenType op,
	                       const string& op_spelling);
	SemValue AnalyzeFunctionalCast(const TypePtr& dest,
	                               const vector<AstExprPtr>& arguments);
	SemValue AnalyzeSizeof(const AstExpr& expr);
	// PA20 5.3.7: noexcept(expression) over resolved unwind facts.
	SemValue AnalyzeNoexcept(const AstExpr& expr);
	void FillFunctionSetValue(const ScopeBinding& binding,
	                          const NamedTypeInfo* member_class,
	                          SemValue& value);
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
	// PA17: the class named by a qualified member-call name's prefix
	// (`d.Base::f()`), checked to be the object's class or a base.
	const NamedTypeInfo* ResolveMemberQualifier(
		const AstName& name, const NamedTypeInfo* object_entity);
	// PA17: `qualified` suppresses dynamic dispatch (10.3p15 explicit
	// scope qualification calls the named function directly). PA21:
	// `explicit_part` carries an explicit member template-id's
	// argument list.
	SemValue AnalyzeMethodCall(SemValue object, const ScopeBinding& binding,
	                           const vector<AstExprPtr>& arguments,
	                           bool qualified = false,
	                           const AstNamePart* explicit_part = 0);
	SemValue AnalyzeStaticMethodCall(const AstExpr& expr,
	                                 const ScopeBinding& binding,
	                                 const AstNamePart* explicit_part = 0);
	SemValue AnalyzeStaticMemberValue(const ScopeBinding& binding,
	                                  const string& written);

	// --- PA16 allocation expressions (sem_new.cpp) ---
	SemValue AnalyzeNew(const AstExpr& expr);
	SemValue AnalyzeNewArray(const AstExpr& expr, size_t bound_item);
	SemValue AnalyzeDelete(const AstExpr& expr);
	SemValue MakeAllocationCall(const char* name, vector<SemValue> args,
	                            const TypePtr& result_type,
	                            bool& unwind_no);
	SemValue MakeSizeLiteral(unsigned long long size);
	SemValue AnalyzeStringUdl(const AstExpr& expr);
	SemValue AnalyzeNumericUdl(const AstExpr& expr);
	SemValue FoldObjectlessConstant(const ScopeBinding& binding);
	// --- PA15 operator overloading (sem_operator.cpp) ---
	void CollectOperatorCandidates(const string& op_name,
	                               const vector<SemValue>& operands,
	                               bool member_only,
	                               vector<OperatorCandidate>& out);
	// PA18: deduced function-template specializations of `binding`
	// against the argument list join the candidate set.
	void AppendTemplateCandidates(const ScopeBinding& binding,
	                              const vector<SemValue>& args,
	                              vector<OperatorCandidate>& out,
	                              std::set<const void*>& seen,
	                              const AstNamePart* explicit_part = 0);
	// PA21: member (operator) templates deduce against the explicit
	// operands; the object binds as the implicit parameter in ranking.
	void AppendMemberTemplateCandidates(const ScopeBinding& binding,
	                                    const vector<SemValue>& operands,
	                                    vector<OperatorCandidate>& out,
	                                    std::set<const void*>& seen);
	// PA18 13.4p2: extends a target-directed function set with the
	// specializations deduced from the destination type.
	void AddTargetDeducedOverloads(SemValue& value, const TypePtr& dest);
	// PA18 13.4: function-set arguments deduce against every ranked
	// candidate's parameter types; refreshes `sources` when any grew.
	void DeduceFunctionSetArguments(vector<SemValue>& args,
	                                const vector<TypePtr>& candidates,
	                                vector<ConversionSource>& sources);
	void NamedCallMinArity(const ScopeBinding& binding,
	                       const vector<TypePtr>& candidates,
	                       const vector<const FunctionSpecialization*>& specs,
	                       size_t ordinary, vector<size_t>& min_arity,
	                       vector<bool>& is_template);
	SemValue SynthesizeDefaultArgument(const ScopeBinding& chosen,
	                                   size_t slot, size_t index,
	                                   const TypePtr& param);
	// 13.4: applies a target-selected overload to the value's node
	// (deduced specializations re-target its identity).
	void ApplySelectedOverload(SemValue& value,
	                           const ImplicitConversion& conv);
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

