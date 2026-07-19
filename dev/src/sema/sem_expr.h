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
	// PA22 presentation fold: whether an object of class `type`
	// contextually converts to a known constant bool (an
	// integral_constant-style conversion operator returning a
	// constant); false when unknown.
	virtual bool TryConstantClassBool(const TypePtr& type, bool& out)
	{
		(void)type;
		(void)out;
		return false;
	}
	virtual Scope* CurrentScope() = 0;
	// PA34 __func__/__builtin_FUNCTION: the name of the function whose
	// body is being analyzed ("" at namespace scope).
	virtual string CurrentFunctionName()
	{
		return string();
	}
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
	// PA25: demand-synthesis of the selected constructor's body for a
	// context that always keeps the call (new-expressions).
	virtual void EnsureSpecialCtorHost(const ClassInfo& cls,
	                                   int index) = 0;
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
	// when deduction fails); explicit template-id arguments pre-bind
	// the leading parameters (14.8.1).
	virtual const FunctionSpecialization* DeduceFunctionTemplateFromTarget(
		TemplateInfo& tmpl, const TypePtr& target,
		const AstNamePart* explicit_part = 0) = 0;
	// PA18 14.7.1p2: a resolved use selected this specialization - the
	// analyzer reports every winner so the host instantiates its body
	// (deduction alone composes only the signature).
	virtual void OnSpecializationOdrUsed(
		const FunctionSpecialization* spec) = 0;
	// Marks an unevaluated operand (3.2p2: no odr-use inside); returns
	// the previous state for restoring.
	virtual bool SwapUnevaluatedOperand(bool active) = 0;
	// PA25 5.2.8: the declared std::type_info class entity; throws
	// when the program has not declared it.
	virtual const NamedTypeInfo* StdTypeInfoEntity() = 0;
	// PA19 14.7.1: a reference to a static data member of a
	// class-template specialization - the host instantiates the
	// specialization's registered out-of-class definitions on this
	// demand (folding reads demand more narrowly, see the host).
	virtual void OnStaticMemberReferenced(const ScopeBinding& binding,
	                                      bool folding_read)
	{
		(void)binding;
		(void)folding_read;
	}
	// PA23 14.7.1: a constructed temporary is an object of the class
	// like a declared variable; the host gives the specialization
	// chain's static-data-member definitions their storage.
	virtual void OnClassObjectMaterialized(const NamedTypeInfo* info)
	{
		(void)info;
	}
	// PA21: whether the host is binding an instantiated pattern body
	// (folded static-member reads there leave no storage).
	virtual bool IsInstantiating() const { return false; }
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
	// PA34: a pack-expanded type-id pattern (builtin trait arguments);
	// false when the pattern mentions no expandable pack.
	virtual bool ExpandPackTypeId(const AstTypeId& pattern,
	                              vector<TypePtr>& out) = 0;
	// PA24 lambdas (sem_lambda.cpp): analyzes a lambda-expression (the
	// binder owns closure/function synthesis).
	virtual SemValue AnalyzeLambda(const AstExpr& expr) = 0;
	// PA34 templated lambdas: deduces the template head from the
	// immediate-invocation arguments and yields the bound closure.
	virtual SemValue AnalyzeTemplatedLambdaInvoke(
		const AstExpr& expr, const vector<SemValue>& args) = 0;
	// PA29 GNU statement expressions: the binder binds the compound
	// statement in place; the last expression statement's value is
	// the expression's value.
	virtual SemValue AnalyzeStatementExpression(const AstExpr& expr) = 0;
	// PA24: rewrites the use of an enclosing function-local entity
	// inside an open lambda body into a closure-field access. False
	// outside lambda bodies (the ordinary id path proceeds).
	virtual bool TryCaptureUse(const ScopeBinding& binding,
	                           SemValue& out) = 0;
	// The `this` value expression: the parameter-backed id, or the
	// captured-this field inside an open lambda body.
	virtual SemNodePtr ThisValueNode() = 0;
	// PA24: the synthesized function behind a captureless closure
	// class (false for ordinary classes).
	virtual bool CapturelessClosureFunction(const NamedTypeInfo* cls,
	                                        const Scope*& owner,
	                                        string& name,
	                                        TypePtr& type) = 0;
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
	// PA23 14.8.1: the explicit template-id arguments of a set that
	// could not resolve fully (`&X::create<Service, Owner>`); target
	// -directed deduction pre-binds them. `fn_set_addressed` marks a
	// set spelled under & - the selected member forms the pointer
	// directly (5.3.1p6 with 13.4).
	const AstNamePart* fn_explicit_part = 0;
	bool fn_set_addressed = false;
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
	// PA24: a braced-init-list call argument awaiting its list
	// -initialization target (13.3.3.1.5). `type` and `node` stay null
	// until ApplyConversion builds the initialization for the selected
	// parameter from the analyzed elements.
	bool braced_list = false;
	vector<SemValue> list_values;
};

// One composed member-call candidate set (sem_member.cpp): ordinary
// overloads first, then deduced member-template specializations, with
// the implicit-object-augmented signatures overload ranking uses.
struct MemberCandidateSet
{
	vector<TypePtr> declared;
	vector<const FunctionSpecialization*> specs;
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	vector<bool> is_template;
	size_t ordinary = 0;
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
	// PA25 15.1: a throw-expression (null operand: rethrow); the
	// binder's throw-statement path enters here too.
	SemValue AnalyzeThrow(const AstExpr* operand);
	// PA33 __builtin_va_arg(list, T): the SysV vararg fetch.
	SemValue AnalyzeVaArg(const AstExpr& expr);
	// PA34 builtin type trait ( type-id-list ) -> bool constant
	// (sem_trait.cpp).
	SemValue AnalyzeBuiltinTrait(const AstExpr& expr);
	// PA34 would-it-compile traits (__is_constructible family):
	// evaluated by probing the initialization/conversion/assignment
	// machinery over declval surrogates (sem_trait.cpp).
	bool EvaluateSemaProbeTrait(const std::string& name,
	                            const vector<TypePtr>& types);
	bool ProbeTraitConstructible(const TypePtr& target,
	                             const vector<TypePtr>& arg_types,
	                             bool& no_throw, bool& trivial);
	bool ProbeTraitConvertible(const TypePtr& from, const TypePtr& to);
	bool ProbeTraitAssignable(const TypePtr& lhs, const TypePtr& rhs,
	                          bool& no_throw, bool& trivial);
	bool ProbeTraitDestructible(const TypePtr& target, bool& no_throw,
	                            bool& trivial);
	// PA34 fold-expressions over constant operands (sem_trait.cpp).
	SemValue AnalyzeFold(const AstExpr& expr);
	// A constructed (or aggregate-initialized) temporary of a class
	// type from an argument/braced-init list (5.2.3, 8.5.4); the
	// binder's braced-return path builds through it too. `braced_list`
	// marks a spelled braced form: an empty list over an aggregate
	// with class-typed members takes the field-wise shape.
	SemValue MakeTemporaryObject(const TypePtr& class_type,
	                             const vector<AstExprPtr>& arguments,
	                             bool braced_assign,
	                             bool braced_list = false);

	// 8.5 copy-initialization of a `dest`-typed object, parameter, or
	// return value (references bind): validates the conversion, retypes
	// null pointer literals, and resolves target-directed function
	// sets. Throws when no conversion exists.
	void CopyInitialize(SemValue& value, const TypePtr& dest,
	                    const char* what);

	// Braced initialization of the supported array forms; returns the
	// braced-init-list node (completing unknown bounds via `dest`).
	SemNodePtr AnalyzeBracedInit(const vector<AstExprPtr>& items,
	                             TypePtr& dest);
	// One argument/initializer list with `pattern...` items expanded
	// in place (PA19 14.5.3). `allow_braced` admits braced-init-list
	// arguments as deferred list-initialization values (PA24 8.5.4).
	void AnalyzeArgumentList(const vector<AstExprPtr>& items,
	                         vector<SemValue>& out,
	                         bool allow_braced = false,
	                         size_t from = 0);

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
	// The braced-argument arm of ApplyConversion (PA24 8.5.4).
	void ApplyListInitConversion(SemValue& value,
	                             const ImplicitConversion& conv,
	                             const TypePtr& dest);

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
	                        const vector<const ScopeBinding*>& visible,
	                        const AstNamePart* explicit_part = 0);
	SemValue AnalyzeBuiltinConstantP(const AstExpr& expr);
	SemValue AnalyzeUnary(const AstExpr& expr);
	// The SB_VARIABLE leg of AnalyzeId (true: `value` is complete).
	bool AnalyzeVariableIdValue(const AstExpr& expr,
	                            const ScopeBinding& binding,
	                            const NamedTypeInfo*& member_class,
	                            const string& written, SemValue& value);
	SemValue AnalyzeAddressOf(const AstExpr& expr);
	// PA26: a member-function pointer template parameter use.
	SemValue MemberPointerParamConstant(const ScopeBinding& binding,
	                                    const TypePtr& pm);
	SemValue AnalyzeIncDec(const AstExpr& expr, bool prefix);
	SemValue AnalyzeBinary(const AstExpr& expr);
	SemValue AnalyzeAdditive(const AstExpr& expr, SemValue& lhs,
	                         SemValue& rhs);
	SemValue AnalyzeComparison(const AstExpr& expr, SemValue& lhs,
	                           SemValue& rhs);
	// PA26 5.5: `.*` / `->*` application over non-virtual layouts.
	SemValue AnalyzeMemberPointerBinary(const AstExpr& expr, SemValue& lhs,
	                                    SemValue& rhs);
	SemValue AnalyzeAssignment(const AstExpr& expr);
	SemValue AnalyzeConditional(const AstExpr& expr);
	TypePtr ConditionalResultType(const SemValue& a, const SemValue& b,
	                              EValueCategory& category);
	SemValue AnalyzeSubscript(const AstExpr& expr);
	SemValue AnalyzeMember(const AstExpr& expr);
	// PA27: the base-subobject view of an upcast reference cast
	// (virtual bases ride their carrier entry).
	bool UpcastReferenceView(const TypePtr& dest, const TypePtr& to,
	                         SemValue& value);
	SemValue AnalyzeCastToReference(const TypePtr& dest, SemValue value,
	                                ETokenType op);
	// PA25/PA26 5.2.7: the pointer dynamic_cast forms (downcast and
	// void*); false when the generic cast classification applies.
	bool TryDynamicCastPointer(const TypePtr& to, SemValue& value,
	                           SemValue& result);
	SemValue AnalyzeCastTo(const TypePtr& dest, const AstExpr& operand,
	                       bool has_anno, ETokenType op,
	                       const string& op_spelling);
	SemValue AnalyzeFunctionalCast(const TypePtr& dest,
	                               const vector<AstExprPtr>& arguments);
	SemValue AnalyzeSizeof(const AstExpr& expr);
	// PA20 5.3.7: noexcept(expression) over resolved unwind facts.
	SemValue AnalyzeNoexcept(const AstExpr& expr);
	// PA25 5.2.8: typeid over a type-id or expression operand.
	SemValue AnalyzeTypeid(const AstExpr& expr);
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
	                           const AstNamePart* explicit_part = 0,
	                           size_t args_from = 0);
	SemValue AnalyzeStaticMethodCall(const AstExpr& expr,
	                                 const ScopeBinding& binding,
	                                 const AstNamePart* explicit_part = 0);
	// Composes a member call's candidate set (13.3.1p2-p4): ordinary
	// overloads and deduced member-template specializations with
	// their implicit object parameters.
	void ComposeMethodCandidates(const SemValue& object,
	                             const ScopeBinding& binding,
	                             const vector<SemValue>& args,
	                             const AstNamePart* explicit_part,
	                             MemberCandidateSet& out);
	// PA18 13.4: overloaded/template arguments deduce against every
	// candidate's parameter types before ranking (`shift` aligns the
	// implicit object parameter).
	void AugmentOverloadSetArguments(const vector<TypePtr>& candidates,
	                                 vector<SemValue>& args, size_t shift);
	// The resolved member callee node (spec winners route like
	// namespace-scope specializations; ordinary winners keep their
	// declaring scope and per-overload unwind facts).
	SemNodePtr MakeMemberCalleeNode(const ScopeBinding& binding,
	                                const FunctionSpecialization* spec,
	                                const Scope* owner_scope,
	                                size_t winner, const TypePtr& type,
	                                bool is_method,
	                                const FunctionSpecialization* self_spec);
	// The implicit object argument, projected to the winner's base
	// subobject when the callee belongs to a base class.
	void AttachMethodObjectArgument(SemValue& value, SemValue object,
	                                const ScopeBinding& binding,
	                                const Scope* owner_scope,
	                                const FunctionSpecialization* spec,
	                                const NamedTypeInfo* callee_class);
	SemValue AnalyzeStaticMemberValue(const ScopeBinding& binding,
	                                  const string& written);
	// PA23 14.6.4.1: whether a static-member constant is visible to
	// this read (a value from an instantiated out-of-class definition
	// folds only inside instantiated bodies).
	bool StaticMemberValueFolds(const ScopeBinding& binding);
	// 9.3.1p3: whether a qualified field name inside a member function
	// reads through this.
	bool QualifiedFieldThroughThis(const ScopeBinding& binding,
	                               const NamedTypeInfo* member_class);

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
	// against the argument list join the candidate set. `declared_in`
	// filters to templates declared in one scope (the ADL leg: friend
	// templates may ride a binding owned by a using-declaration).
	void AppendTemplateCandidates(const ScopeBinding& binding,
	                              const vector<SemValue>& args,
	                              vector<OperatorCandidate>& out,
	                              std::set<const void*>& seen,
	                              const AstNamePart* explicit_part = 0,
	                              const Scope* declared_in = 0);
	// The argument-dependent candidates of an unqualified call (3.4.2).
	void AppendAdlCandidates(const string& name,
	                         const vector<SemValue>& args,
	                         vector<OperatorCandidate>& candidates,
	                         std::set<const void*>& seen,
	                         const AstNamePart* explicit_part = 0);
	// PA21 9.4p2: whether every entry of a member function set is
	// static (the set then decays like ordinary functions).
	bool FunctionSetAllStatic(const ScopeBinding& binding);
	// The resolved operator call's operand children (member operators
	// take the object address first).
	void AppendOperatorOperands(bool is_member,
	                            const NamedTypeInfo* member_owner,
	                            vector<SemValue>& operands,
	                            SemNode& call);
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
	// (deduced specializations re-target its identity). `dest` shapes
	// an addressed set's pointer (member pointer targets, PA26).
	void ApplySelectedOverload(SemValue& value,
	                           const ImplicitConversion& conv,
	                           const TypePtr& dest);
	static bool OperatorOperand(const SemValue& value);
	bool TryBinaryOperator(const string& spelling, SemValue& lhs,
	                       SemValue& rhs, SemValue& result);
	bool TryUnaryOperator(const string& spelling, SemValue& operand,
	                      bool postfix, SemValue& result);
	SemValue AnalyzeFunctorCall(SemValue object, const AstExpr& expr,
	                            size_t args_from = 0);
	// PA34 magic-typed hosted builtins (sem_hosted_builtin.cpp):
	// callee forms whose types derive from their arguments.
	bool TryAnalyzeMagicBuiltin(const AstExpr& expr, const string& name,
	                            SemValue& out);
	SemValue MakeBuiltinCallResult(const string& name,
	                               const TypePtr& fn_type,
	                               vector<SemValue>& args, bool no_throw);
	SemValue AnalyzeBuiltinAddressOf(const AstExpr& expr);
	SemValue AnalyzeBuiltinBitCountG(const AstExpr& expr,
	                                 const string& name);
	SemValue AnalyzeBuiltinOverflow(const AstExpr& expr,
	                                const string& name);
	SemValue AnalyzeBuiltinAllocation(const AstExpr& expr,
	                                  const string& name);
	SemValue AnalyzeBuiltinFpclassify(const AstExpr& expr);
	SemValue MakeFunctionNameLiteral(const string& text);
	bool TryAnalyzeAtomicBuiltin(const AstExpr& expr, const string& name,
	                             SemValue& out);
	SemValue AnalyzeBuiltinInvoke(const AstExpr& expr);
	SemValue AnalyzeOffsetof(const AstExpr& expr);
	const ClassField* OffsetofField(const ClassInfo& cls,
	                                const string& name,
	                                unsigned long long& offset);
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

