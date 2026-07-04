#pragma once

#include <map>
#include <set>

#include "sema/class_info.h"
#include "sema/const_eval.h"
#include "sema/decl_binder.h"
#include "sema/sem_expr.h"
#include "sema/sem_node.h"

// The PA12 semantic binder: the PA11 declaration traversal extended
// with overload sets, full statement and expression analysis of
// function bodies, and declaration-order dump recording into a
// SemUnit. One instance binds one translation unit.
class SemBinder : public DeclBinder, public ISemExprHost
{
public:
	SemBinder(TypesModel& model, SemUnit& unit);
	~SemBinder();

	// PA18: the end-of-unit template re-checks run after the forward
	// pass (definition-time sanity, 14.6p7).
	virtual void BindTranslationUnit(const AstDecl& unit);

	// ISemExprHost
	virtual Scope* CurrentScope();
	virtual TypesModel& Model();
	virtual const ScopeBinding* ResolveValue(const AstName& name,
	                                         const NamedTypeInfo*& member_class);
	virtual TypePtr TryResolveCalleeType(const AstName& name);
	virtual TypePtr ResolveCastTypeId(const AstTypeId& type_id);
	virtual bool TryEvaluateConstant(const AstExpr& expr, ConstValue& value);
	// PA20: analyzed-tree constant evaluation (the DeclBinder seam).
	virtual bool TryFullConstant(const AstExpr& expr, ConstValue& out);
	virtual const ScopeBinding* ResolveBuiltinFunction(const string& name);
	virtual ClassRegistry& Classes();
	virtual const ClassInfo* CurrentClass();
	virtual TypePtr CurrentThisType();
	virtual void CheckMemberAccess(const Scope* owner, EMemberAccess access,
	                               const string& what,
	                               const NamedTypeInfo* naming = 0);
	virtual bool InClassContextOrFriend(const NamedTypeInfo* cls);
	virtual SemNodePtr MakeConstructorCall(const ClassInfo& cls,
	                                       int ctor_index, bool base_entry,
	                                       SemNodePtr address,
	                                       vector<SemNodePtr> args);
	virtual SemNodePtr MakeTemporaryDtor(const ClassInfo& cls);
	virtual int ResolveClassCtorHost(const ClassInfo& cls,
	                                 vector<SemValue>& args, bool copy_init,
	                                 const char* what);
	virtual void EnsureAssignSpecial(const NamedTypeInfo* cls_entity,
	                                 size_t overload_index);
	virtual SemNodePtr MakeAggregateTemporary(const ClassInfo& cls,
	                                          vector<SemValue> args);

	// ITypeBuilderHost: decltype over the full PA12 expression subset.
	virtual TypePtr ResolveDecltype(const AstExpr& expr);
	// PA15: type-name resolution checks member access (11.8 subset).
	virtual TypePtr ResolveTypeName(const AstName& name);

protected:
	// DeclBinder seams
	virtual void BindNamespaceBody(const AstDecl& decl, Scope* scope);
	virtual void BindSimpleDeclaration(const AstDecl& decl);
	virtual ScopeBinding& BindFunctionName(const string& name,
	                                       const TypePtr& type,
	                                       bool allow_block);
	virtual void BindFunctionBody(const AstDecl& decl,
	                              const DeclaratorInfo& composed,
	                              const string& name);
	virtual void OnTypeAliasBound(const string& name, const TypePtr& type);
	virtual void OnVariableBound(ScopeBinding& binding,
	                             const AstInitializer* init,
	                             const DeclSpecifierInfo& specs);
	virtual void OnFunctionDeclared(ScopeBinding& binding,
	                                const TypePtr& type,
	                                const DeclSpecifierInfo* specs = 0,
	                                const DeclaratorInfo* composed = 0,
	                                bool pure = false);
	virtual void BindAnonymousUnionMembers(const AstDecl& decl,
	                                       const TypePtr& type,
	                                       const Scope& union_scope);
	virtual void BindStatement(const AstStmt& stmt);
	virtual string TypeDisplayName(const string& key,
	                               const string& name) const;
	virtual string AnonymousTypeName(const AstDecl& decl);

	// --- PA15 class machinery (sem_class.cpp) ---
	virtual void OnClassOpened(const AstDecl& decl, NamedTypeInfo* info,
	                           Scope* scope);
	virtual void BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
	                            Scope* scope);
	virtual void CompleteClass(const AstDecl& decl, NamedTypeInfo* info,
	                           Scope* scope,
	                           const std::vector<TypePtr>& fields);
	virtual void BindSpecialMember(const AstDecl& decl);
	virtual void BindBitFieldDeclaration(const AstDecl& decl);
	virtual void BindFriendDeclaration(const AstDecl& decl);
	virtual void CheckQualifiedDefinitionScope(const Scope* declaring);
	virtual void BindQualifiedDeclarator(const DeclSpecifierInfo& specs,
	                                     const AstInitDeclarator& declarator,
	                                     const DeclaratorInfo& composed);

private:
	// --- dump recording ---
	SemNode* AppendItem(SemNodePtr node);
	SemNode* AppendItem(ESemNodeKind kind);
	string QualifiedScopePath(const Scope* scope) const;

	// --- statements ---
	void BindCompoundStatement(const AstStmt& stmt);
	void BindDeclarationStatement(const AstStmt& stmt);
	void BindExpressionStatement(const AstStmt& stmt);
	void BindReturnStatement(const AstStmt& stmt);
	SemNodePtr WrapReturnValue(SemValue value, const TypePtr& bare);
	void BindCondition(const AstCondition& condition, bool for_switch);
	void BindConditionDeclaration(const AstCondition& condition,
	                              bool for_switch);
	void BindIfStatement(const AstStmt& stmt);
	void BindWhileStatement(const AstStmt& stmt);
	void BindDoStatement(const AstStmt& stmt);
	void BindForStatement(const AstStmt& stmt);
	void BindSwitchStatement(const AstStmt& stmt);
	void BindLabelStatement(const AstStmt& stmt);

	// --- initialization and implicit constructors ---
	void AnalyzeVariableInit(SemNode& item, ScopeBinding& binding,
	                         const AstInitializer* init);
	// PA20: evaluates a constexpr (or engine-foldable const) object's
	// analyzed initializer: records scalar values on the binding,
	// stamps folded initializers for the lowering, stores object
	// images for later constant reads, and enforces 7.1.5p9 (a
	// constexpr object requires a constant initializer).
	void FinishConstexprObject(SemNode& item, ScopeBinding& binding,
	                           bool is_constexpr);
	// 9.4.2p3: an initializer-less storage definition adopts the
	// member's constant in-class initializer (folded literal or the
	// analyzed scalar initializer) as its rendered value.
	void AdoptInClassInitializer(SemNode& item,
	                             const ScopeBinding& member,
	                             bool initializer_less,
	                             bool inclass_object);
	// 8.5.2: the synthesized per-code-unit element list of a string
	// -literal array initializer.
	SemNodePtr StringLiteralArrayInit(ScopeBinding& binding,
	                                  const SemNode& literal,
	                                  const TypePtr& literal_type);
	void EmitConstructorAction(SemNode& item, const string& var_name,
	                           const TypePtr& type);
	const string& EnsureDefaultConstructor(const TypePtr& type,
	                                       TypePtr& ctor_type);

	// --- PA15 class machinery (sem_class.cpp) ---
	struct DeferredBody;
	ClassInfo* OpenClass() const;
	Scope* EnclosingNamespace();
	void RecordMemberField(ScopeBinding& binding,
	                       const AstInitializer* init,
	                       const DeclSpecifierInfo& specs);
	// PA20: bound completion and constant evaluation of an
	// object-valued static member's in-class initializer.
	void RecordStaticMemberObject(ScopeBinding& binding,
	                              const AstInitializer* init,
	                              const DeclSpecifierInfo& specs);
	void BindFriendFunction(const AstDecl& decl, ClassInfo& cls);
	// 14.5.4p5: a template-id friend pins a specialization of an
	// existing template instead of declaring a new function.
	void BindTemplateIdFriend(Scope* target, const string& name,
	                          const TypePtr& declared);
	// PA16: a conversion-function member declaration (12.3.2).
	void BindConversionFunction(const AstDecl& decl, ClassInfo& cls,
	                            const AstNamePart& part);
	void BindMemberFunctionBody(const AstDecl& decl,
	                            const DeclaratorInfo& composed,
	                            const string& name);
	void FlushDeferredBodies();
	void AnalyzeDeferredBody(const DeferredBody& body);
	// 14.7.1: an instantiated member whose body failed to bind keeps a
	// poisoned weak definition; demanding it reports the stored error.
	void AppendPoisonedBody(const DeferredBody& body, const string& what);
	// Publishes a bound body's derived non-throwing fact so callers
	// resolved afterwards skip unwind regions around calls to it.
	void PublishBodyUnwindFact(const DeferredBody& body,
	                           ESpecialFunction special, SemNode& node);
	void BindQualifiedSpecialMember(const AstDecl& decl,
	                                const AstName& id);
	void BindQualifiedSpecialMemberInner(const AstDecl& decl,
	                                     const AstName& id,
	                                     bool defaulted);
	void BindQualifiedDestructor(const AstDecl& decl, Scope* declaring,
	                             ClassInfo& cls, bool defaulted);
	void BindQualifiedConversionFunction(const AstDecl& decl,
	                                     const AstNamePart& part,
	                                     Scope* declaring, ClassInfo& cls);
	Scope* MakeSpecialMemberScope(const string& name,
	                              const DeclaratorInfo& composed,
	                              ClassInfo& cls);
	TypePtr MethodAdjustedType(const ClassInfo& cls, const TypePtr& member);
	SemNodePtr BuildFunctionNode(const DeferredBody& body,
	                             ESpecialFunction special);
	// The callee destroys its by-value class parameters at scope exit;
	// the action attaches as a child of the parameter node.
	void AttachParameterDtor(SemNode& parameter);
	void AnalyzeMemberInits(const DeferredBody& body, SemNode& item);
	void AnalyzeDtorEpilogue(const ClassInfo& cls, SemNode& item);
	void AppendMemberInit(const ClassInfo& cls, const ClassField& field,
	                      const AstInitializer* init,
	                      vector<SemNodePtr>& out);
	void AppendClassMemberInit(const ClassField& field,
	                           const ClassInfo& member_cls,
	                           const AstExpr* braced,
	                           const vector<const AstExpr*>& args,
	                           vector<SemNodePtr>& out);
	void AppendFieldDefaultInit(const ClassInfo& cls,
	                            const ClassField& field,
	                            vector<SemNodePtr>& out);
	void AppendBaseDefaultInit(const ClassInfo& cls,
	                           vector<SemNodePtr>& out);
	void AppendElidedCtorDemand(const ClassInfo& cls, bool base_entry,
	                            vector<SemNodePtr>& out);
	void AppendArrayMemberInit(const ClassField& field,
	                           const AstExpr* braced,
	                           vector<SemNodePtr>& out);
	SemValue ZeroValue(const TypePtr& type);
	void CheckListInitNarrowing(const SemValue& value, const TypePtr& dest);
	SemNodePtr ThisObjectExpr();
	SemNodePtr ThisFieldExpr(const ClassField& field);
	SemNodePtr ThisBaseAddress(const ClassInfo& cls);
	SemNodePtr AddressOfNode(SemNodePtr operand);
	SemNodePtr SubscriptNode(SemNodePtr array, unsigned long long index);
	SemNodePtr MemberAssignAction(const ClassField& field,
	                              SemNodePtr lhs, SemValue value);
	// Overload resolution over the class's declared constructors;
	// applies conversions and synthesizes default arguments. Returns -1
	// when initialization uses the implicit default constructor.
	int ResolveClassConstructor(const ClassInfo& cls,
	                            vector<SemValue>& args, bool copy_init,
	                            const char* what);
	// 13.3.1.4 copy-initialization: converts a differing-class source
	// through its conversion functions when no converting constructor
	// is usable (true when the argument was converted in place).
	bool TryCopyInitSourceConversion(const ClassInfo& cls, SemValue& arg);
	// The synthesized implicit default constructor / destructor
	// definitions, built on first demand into unit_.deferred (an
	// out-of-class `= default` builds them as strong definitions).
	void EnsureImplicitDefaultCtor(const ClassInfo& cls,
	                               bool out_of_class = false);
	void EnsureImplicitDtor(const ClassInfo& cls,
	                        bool out_of_class = false);
	void EnsureInheritedCtor(const ClassInfo& cls, int index);

	// --- PA16 copy/move special members (sem_special.cpp) ---
	// Implicit declaration at class completion (12.8) and the
	// demand-driven synthesis of copy/move constructor and assignment
	// definitions.
	void DeclareImplicitSpecialMembers(ClassInfo& cls);
	void DeclareImplicitAssign(ClassInfo& cls, bool is_move, bool deleted);
	void EnsureSpecialCtor(const ClassInfo& cls, int index,
	                       bool out_of_class = false);
	void BuildAssignSpecial(ClassInfo& cls, size_t overload_index,
	                        bool out_of_class);
	// Recomputes the user-provided-constructor fact after an
	// out-of-class `= default` re-classifies a declaration.
	void RecomputeUserCtorFact(ClassInfo& cls);
	struct SpecialBodyContext;
	SemNodePtr SourceObjectExpr(Scope* fn_scope, const string& name,
	                            const TypePtr& class_type);
	SemNodePtr SourceFieldExpr(const SemNode& source_proto,
	                           const ClassField& field,
	                           EValueCategory category);
	SemNodePtr StorageCopyAction(const ClassInfo& cls,
	                             const SemNode& source_proto,
	                             unsigned long long span,
	                             unsigned long long alignment);
	// The leading trivially copyable storage prefix of the class for a
	// copy (is_move false) or move (true) transfer: returns the byte
	// span (0 when none; cls.size when the whole object is trivial) and
	// the span alignment, and sets `first_suffix` to the first field
	// row needing individual lowering.
	unsigned long long TrivialStoragePrefix(const ClassInfo& cls,
	                                        bool is_move, bool assign_form,
	                                        unsigned long long& alignment,
	                                        size_t& first_suffix);
	void AppendBaseTransfer(const ClassInfo& cls, bool is_move,
	                        bool assign_form, const SemNode& source_proto,
	                        vector<SemNodePtr>& out);
	void AppendMemberArrayTransfer(const ClassInfo& member,
	                               const ClassField& field,
	                               const SemNode& source_proto,
	                               EValueCategory category,
	                               vector<SemNodePtr>& out);
	void AppendTransferActions(const ClassInfo& cls, bool is_move,
	                           bool assign_form, const SemNode& source_proto,
	                           vector<SemNodePtr>& out);
	virtual void BindInheritingConstructors(Scope* base_scope);
	SemNodePtr MakeDestructorCall(const ClassInfo& cls, bool base_entry,
	                              SemNodePtr address);

	// --- PA18 templates (sem_template.cpp) ---
	// Capture seams: template declarations are recorded, not analyzed;
	// instantiation re-binds the stored AST on demand.
	virtual void BindTemplateDeclaration(const AstDecl& decl);
	virtual void BindExplicitInstantiation(const AstDecl& decl);
	virtual const ScopeBinding* ResolveTemplateIdBinding(
		const AstNamePart& part, Scope* prefix);
	void CaptureClassTemplate(const AstDecl& decl, const AstDecl& inner,
	                          bool definition);
	bool QualifierIsNamespacePath(const AstName& name);
	void CaptureQualifiedClassTemplate(const AstDecl& decl,
	                                   const AstDecl& inner);
	// Returns the merged or fresh record; `as_friend` captures a
	// hidden (ADL-only) namespace-scope friend template.
	TemplateInfo* CaptureFunctionTemplate(const AstDecl& decl,
	                                      const AstDecl& inner,
	                                      bool as_friend = false);
	// --- PA21 member templates (sem_member_template.cpp) ---
	// Class-scope template-declaration dispatch (member function /
	// constructor / class / alias / friend templates).
	void BindMemberTemplateDeclaration(const AstDecl& decl,
	                                   const AstDecl& inner);
	void CaptureMemberFunctionTemplate(const AstDecl& decl,
	                                   const AstDecl& inner);
	void CaptureConstructorTemplate(const AstDecl& decl,
	                                const AstDecl& inner);
	// A conversion-function template (14.8.2.3): recorded on the
	// ClassInfo for destination-directed deduction.
	void CaptureConversionFunctionTemplate(const AstDecl& decl,
	                                       const AstDecl& inner,
	                                       ClassInfo& cls);
	void DeduceOneConversionTemplate(TemplateInfo& tmpl,
	                                 const TypePtr& dest, ClassInfo& cls);
	// A dependent alias-template use kept as a TEMPLATE_SPEC pattern:
	// re-resolves under the match's alias scope with the deduced
	// bindings substituted (14.5.7p2).
	TypePtr ResolveDependentAliasUse(const TypePtr& pattern,
	                                 Scope* alias_scope,
	                                 const vector<TemplateArg>& bound);
	// A friend template declaration/definition inside a class (11.3):
	// captures into the enclosing namespace, hidden until a matching
	// namespace-scope declaration appears.
	void BindFriendTemplate(const AstDecl& decl, const AstDecl& inner);
	// Constructor-template support for construction sites: deduces the
	// class's constructor templates against the argument list and
	// synthesizes ClassCtor entries for the winners.
	void AppendCtorTemplateCandidates(const ClassInfo& cls,
	                                  vector<SemValue>& args,
	                                  vector<TypePtr>& candidates,
	                                  vector<size_t>& min_arity,
	                                  vector<size_t>& positions,
	                                  vector<bool>* is_template = 0);
	// The ClassCtor entry (existing or fresh, body instantiated) for a
	// deduced constructor-template specialization.
	int EnsureCtorTemplateEntry(ClassInfo& cls,
	                            const FunctionSpecialization* spec);
	// An out-of-class member-template definition qualified by a class
	// (or instantiated specialization) scope: captures/merges in that
	// scope.
	void CaptureQualifiedMemberTemplate(const AstDecl& decl,
	                                    const AstDecl& inner,
	                                    Scope* declaring);
	void CaptureQualifiedMemberClassTemplate(const AstDecl& decl,
	                                         const AstDecl& inner);
	// The instantiated (weak, demand-emitted) body of a selected
	// constructor-template specialization.
	void InstantiateCtorTemplateBody(ClassInfo& cls, int index);
	// Collects the type-parameter list of a template head (throws on
	// the out-of-scope parameter forms).
	static void CollectTemplateParams(const AstDecl& decl,
	                                  vector<TemplateParam>& params);
	// An out-of-class definition of a class-template member (function,
	// static data member, nested class, special member): recorded on
	// the owner template and instantiated for every existing
	// specialization.
	void RegisterTemplateMember(const AstDecl& decl, const AstName& id);
	// The class template a qualified member-definition id names;
	// `tmpl_part` receives the index of its template-id (or plain
	// name) component.
	TemplateInfo* ResolveMemberOwnerTemplate(const AstName& id,
	                                         size_t& tmpl_part);
	// Resolves the argument list of one template-id part against
	// `tmpl` (type-id and constant-value arguments; defaults fill the
	// tail).
	vector<TemplateArg> ResolveTemplateArgumentList(TemplateInfo& tmpl,
	                                                const AstNamePart& part);
	// A SCOPE_TEMPLATE_PARAMS scope under the template's declaring
	// scope with each parameter name aliased to its argument (type
	// aliases and constant-value bindings).
	Scope* MakeArgumentAliasScope(const TemplateInfo& tmpl,
	                              const vector<TemplateArg>& args);
	// One parameter-name alias in such a scope (type alias, objectless
	// constant, or PA21 template-name alias).
	void BindParamAlias(Scope& scope, const TemplateParam& param,
	                    const TemplateArg& arg);
	// A parameter pack's alias from an explicit element run (the
	// deduced slot form of a partial specialization).
	void BindPackAliasElements(Scope& scope, const TemplateParam& param,
	                           const vector<TemplateArg>& elements);
	// --- PA21 alias templates and template-template parameters ---
	void CaptureAliasTemplate(const AstDecl& decl, const AstDecl& inner);
	// The substituted type of one alias-template-id use (cached per
	// argument key; 14.5.7: aliases have no specializations of their
	// own).
	const ScopeBinding* ResolveAliasTemplateId(TemplateInfo& tmpl,
	                                           const vector<TemplateArg>& args);
	// A pattern-scope placeholder template standing for the enclosing
	// template's template-template parameter `index`.
	TemplateInfo* TemplateParamPlaceholder(const TemplateParam& param,
	                                       size_t index);
	// One template-template argument (a template-name re-read from the
	// type-form argument) against a TPK_TEMPLATE parameter.
	TemplateArg ResolveTemplateTemplateArgument(const AstName& name,
	                                            const TemplateParam& param,
	                                            const string& context);
	// --- PA19 value arguments (template_args.cpp) ---
	// Whether abstract pattern bindings are in scope (unevaluable
	// value arguments are then dependent, not ill-formed).
	bool InAbstractTemplateContext() const;
	// The declared type of one value parameter under the earlier
	// parameters' bindings.
	TypePtr ValueParamType(const TemplateParam& param, Scope* bindings);
	// One value argument / defaulted value expression against the
	// parameter's declared type.
	TemplateArg ResolveValueArgument(const AstTemplateArgument& argument,
	                                 const TypePtr& param_type);
	TemplateArg ResolveDefaultValueExpr(const AstExpr& expr,
	                                    const TypePtr& param_type);
	// PA20: the engine evaluates a converted constant expression
	// toward an integral/enum parameter type (5.19p3, 14.3.2).
	bool TryFullValueArgument(const AstExpr& expr,
	                          const TypePtr& param_type, ConstValue& out);
	// The lazily-created binding scope over the already-resolved
	// leading arguments.
	Scope* EnsureArgBindingScope(TemplateInfo& tmpl,
	                             const vector<TemplateArg>& so_far,
	                             Scope*& partial);
	// Defaults fill the remaining parameters of one resolved list.
	void FillDefaultedTail(TemplateInfo& tmpl, size_t cursor,
	                       vector<TemplateArg>& args, Scope*& partial);
	// One source argument against one parameter's kind.
	TemplateArg ResolveOneArgument(TemplateInfo& tmpl,
	                               const TemplateParam& param,
	                               const AstTemplateArgument& argument,
	                               const vector<TemplateArg>& so_far,
	                               Scope*& partial);
	// --- PA19 packs (sem_pack.cpp) ---
	// A parameter pack's alias binding over its run of elements.
	void BindPackAlias(Scope& scope, const TemplateParam& param,
	                   const vector<TemplateArg>& args, size_t begin,
	                   size_t end);
	// Lockstep element count of the packs one expansion mentions.
	size_t PackExpansionLength(const vector<const ScopeBinding*>& packs);
	// A transient scope binding each mentioned pack name to its k-th
	// element.
	Scope* MakePackElementScope(const vector<const ScopeBinding*>& packs,
	                            size_t index);
	// `pattern...` in a template-argument list: per-element
	// re-resolution (or a carried pack-pattern slot in abstract
	// contexts).
	void ExpandTemplateArgumentPack(TemplateInfo& tmpl,
	                                const TemplateParam& param,
	                                const AstTemplateArgument& argument,
	                                vector<TemplateArg>& args,
	                                Scope*& partial);
	// `Base...` in a base clause: per-element base types.
	void ExpandPackBases(const AstBaseSpecifier& base,
	                     std::vector<TypePtr>& out);
	// Aggregate braced init whose items contain pack expansions.
	void AppendExpandedAggregateInit(const ClassInfo& cls,
	                                 const SemNode& target_proto,
	                                 const AstExpr& braced,
	                                 vector<SemNodePtr>& out);
	// sizeof...(name).
	size_t PackSize(const string& name);
	// 14.3p1 definition-time shape checks over a captured signature.
	void ValidateTemplateIdShape(const AstNamePart& part);
	void ValidateSignatureTemplateIds(const AstSpecifierSeq& specifiers,
	                                  const AstDeclarator* declarator);
	// --- PA19 explicit/partial specialization + variable templates
	// (sem_spec.cpp) ---
	void BindExplicitSpecialization(const AstDecl& decl);
	void BindClassExplicitSpecialization(const AstDecl& inner);
	// PA21 14.7.3: explicit specialization of one member of a class
	// -template specialization (member function, static data member,
	// constructor).
	void BindMemberExplicitSpecialization(const AstDecl& inner,
	                                      const AstName& id);
	void BindFunctionExplicitSpecialization(const AstDecl& inner);
	void BindVariableExplicitSpecialization(const AstDecl& inner);
	void CaptureVariableTemplate(const AstDecl& decl,
	                             const AstDecl& inner);
	void RegisterVariablePartial(const AstDecl& decl,
	                             const AstDecl& inner);
	void RegisterClassPartial(const AstDecl& decl, const AstDecl& inner);
	vector<TemplateArg> ComposePartialPattern(
		TemplateInfo& primary, const vector<TemplateParam>& params,
		const AstNamePart& part);
	int MatchPartialSpecialization(TemplateInfo& tmpl,
	                               const vector<TemplateArg>& args,
	                               vector<TemplateArg>& bound);
	// PA21: match-time re-resolution of deferred pattern type slots.
	bool CheckDependentPatternSlots(TemplateInfo& tmpl,
	                                const PartialSpecialization& partial,
	                                const vector<TemplateArg>& args,
	                                const vector<TemplateArg>& bound);
	const ScopeBinding* ResolveVariableTemplateId(TemplateInfo& tmpl,
	                                              const AstNamePart& part);
	void InstantiateClassFromPartial(TemplateInfo& tmpl,
	                                 ClassSpecialization& spec,
	                                 int partial_index,
	                                 const vector<TemplateArg>& bound);
	// Structural deduction of a pattern argument list against concrete
	// arguments (template_deduce.cpp).
	bool DeduceTemplateArgs(const vector<TemplateArg>& pattern,
	                        const vector<TemplateArg>& args,
	                        vector<TemplateArg>& bound,
	                        bool allow_trailing = true);
	// 14.5.5.2 (subset): whether partial specialization `a` is at
	// least as specialized as `b` (template_deduce.cpp).
	bool PartialAtLeastAsSpecialized(const PartialSpecialization& a,
	                                 const PartialSpecialization& b);
	// DeduceFunctionTemplate stages (template_deduce.cpp): explicit
	// argument binding, one fixed parameter, one pack element.
	bool BindExplicitDeductionArgs(TemplateInfo& tmpl,
	                               const AstNamePart& part,
	                               vector<TemplateArg>& bound,
	                               vector<TemplateArg>& pack_elements);
	bool DeduceFixedParameter(const TypePtr& pattern, const SemValue& arg,
	                          vector<TemplateArg>& bound);
	bool DeducePackElement(const TypePtr& pattern, const SemValue& arg,
	                       size_t pack_index, vector<TemplateArg>& bound,
	                       TemplateArg& element);
	// `pattern...` in an argument/initializer list (ISemExprHost).
	virtual bool ExpandPackExpression(const AstExpr& pattern,
	                                  vector<SemValue>& out);
	// `Args... args` in a parameter clause (ITypeBuilderHost).
	virtual bool ExpandPackParameter(const AstParameter& parameter,
	                                 std::vector<ParameterInfo>& out);
	// PA21: the unexpanded element pattern of `P...` inside an
	// abstract template pattern (ITypeBuilderHost).
	virtual bool ComposeAbstractPackParameter(const AstParameter& parameter,
	                                          ParameterInfo& out);
	// PA21: an abstract-pattern array bound naming a value parameter
	// (ITypeBuilderHost).
	virtual bool AbstractArrayBound(const AstExpr& expr, int& param);
	// The most recent clause expansion (one pack parameter per clause
	// in the slice): the declared pack name and its expanded slots,
	// consumed right after signature composition to bind the function
	// -scope pack binding.
	struct PackParamRecord
	{
		string name;
		vector<string> names;
		vector<TypePtr> types;
	};
	PackParamRecord last_pack_param_;
	// Binds (or completes) the pack-parameter binding in `scope` from
	// last_pack_param_.
	void BindCapturedPackParameter(Scope* scope);
	// The specialization record for `args`, instantiating the class
	// body on demand when the definition is available.
	ClassSpecialization* EnsureClassSpecialization(
		TemplateInfo& tmpl, const vector<TemplateArg>& args);
	// PA21: deferred-body helpers (14.7.1p4).
	void InstantiateSpecializationBody(TemplateInfo& tmpl,
	                                   ClassSpecialization& spec);
	void InstantiateClassSpecialization(TemplateInfo& tmpl,
	                                    ClassSpecialization& spec);
	// Instantiates the registered out-of-class member definitions that
	// are ready (definition and specialization both seen).
	void InstantiateReadyMembers(TemplateInfo& tmpl);
	// PA21: the same for member definitions registered on partial
	// specializations.
	void InstantiateReadyPartialMembers(TemplateInfo& tmpl);
	void InstantiateMemberDefinition(TemplateInfo& tmpl,
	                                 ClassSpecialization& spec,
	                                 size_t member_index);
	// PA19 14.7.1p8: static-data-member definitions instantiate on
	// demand (odr-use, or definition of an object of the
	// specialization type).
	void InstantiateStaticMembers(TemplateInfo& tmpl,
	                              ClassSpecialization& spec,
	                              const string* name);
	ClassSpecialization* FindSpecializationRecord(
		const NamedTypeInfo* entity);
	void DemandSpecializationStatics(const NamedTypeInfo* entity);
	// Saved-and-cleared binder state around one instantiation; the
	// destructor restores it (exception-safe). Defined at the end of
	// this header (it captures the binder's private state).
	struct InstantiationContext;
	// DK_CLASS dispatch (14.7.1p1: a member-class definition of an
	// instantiated class defers; a qualified class-name defines a
	// nested class out of class).
	virtual void BindClassDeclaration(const AstDecl& decl);
	virtual void EnsureTypeCompleteness(const NamedTypeInfo* info);

	// --- PA18 function templates (template_deduce.cpp) ---
	// The shared positional placeholder type for deduction patterns.
	TypePtr PlaceholderType(size_t index);
	// Composes a function-template declarator with the parameters
	// bound to the positional placeholders; false when the full
	// signature has dependent qualified forms (per-parameter patterns
	// still fill, with null non-deduced entries).
	bool ComposeFunctionPattern(const vector<TemplateParam>& params,
	                            Scope* declaring, const AstDecl& inner,
	                            TypePtr& full,
	                            vector<TypePtr>& param_patterns,
	                            vector<bool>& pattern_packs);
	// Composes the abstract signature pattern (lazily, cached).
	void EnsureFunctionPattern(TemplateInfo& tmpl);
	// Whether a new declaration re-declares `tmpl` (positional
	// parameter identity over the composed pattern).
	// `match_head_spelling=false` relaxes the 14.5.6.1p6 template-head
	// comparison for out-of-class definition pairing.
	bool SameFunctionTemplateSignature(TemplateInfo& tmpl,
	                                   const AstDecl& decl,
	                                   const AstDecl& inner,
	                                   bool match_head_spelling = true);
	// 7.3.3p15 hiding between an own member template and an imported
	// one: positional pattern identity over the composed patterns.
	bool SameImportedTemplateSignature(TemplateInfo& own,
	                                   TemplateInfo& other);
	// The specialization for an explicit/deduced argument list,
	// composing the concrete signature on first use (and the body once
	// the definition is available). `slots` (one entry per parameter,
	// pack slots carrying their runs) keys and binds multi-pack
	// deduction results whose flattened list is ambiguous.
	FunctionSpecialization* EnsureFunctionSpecialization(
		TemplateInfo& tmpl, const vector<TemplateArg>& args,
		const vector<TemplateArg>* slots = 0);
	void InstantiateFunctionBody(TemplateInfo& tmpl,
	                             FunctionSpecialization& spec);
	// PA21: the implicit object parameter of a member-template body.
	void AttachMemberBodyThis(const ClassInfo& cls,
	                          const TypePtr& declared, Scope* fn_scope,
	                          SemNode& item);
	// The instantiated body's parameter nodes (slot-name fallbacks and
	// explicit-definition redirects included).
	void AttachBodyParameters(TemplateInfo& tmpl,
	                          FunctionSpecialization& spec,
	                          const DeclaratorInfo& composed,
	                          Scope* fn_scope, SemNode& item);
	// An explicit specialization's renamed parameters redirect onto
	// the primary declaration's slot names.
	void RedirectExplicitSlotNames(const AstDeclarator& primary,
	                               Scope* fn_scope,
	                               vector<string>& slot_names);
	void InstantiatePendingFunctions(TemplateInfo& tmpl);
	// Pre-binds the declared parameters into the capture scope so a
	// trailing-return decltype (which composes before the clause,
	// 8.3.5p2) can name them.
	void PreBindDeclaredParameters(const AstDeclarator* declarator);
	// PA21: the declared parameter names of a declarator (slot-name
	// fallback recording).
	void CollectDeclaredParamNames(const AstDeclarator& declarator,
	                               vector<string>& out);
	// A fully explicit function template-id (`f<int>`): resolves the
	// specialization named by `part` over the templates of `binding`.
	const ScopeBinding* ResolveFunctionTemplateId(
		const ScopeBinding& binding, const AstNamePart& part);
	void BindExplicitFunctionInstantiation(const AstDecl& inner,
	                                       bool is_extern = false);
	// Definition-time template sanity (sem_template_check.cpp):
	// parameter-shadow errors throw; unresolved non-dependent names
	// re-check at the end of the unit.
	void CheckTemplateDefinitionSanity(TemplateInfo& tmpl);
	void FinishTemplateChecks();
	void CheckMemberDefinitionAgainstPattern(
		const TemplateInfo& tmpl, const AstDecl& inner,
		const vector<TemplateParam>& def_params);
	// 14.6.2p1: whether a base-clause name mentions a template
	// parameter of the current instantiation context.
	bool BaseClauseIsDependent(const AstName& name);
	bool NameMentionsAny(const AstName& name,
	                     const std::set<string>& params);
	// PA21: whether a qualified member-definition name's qualifier
	// mentions the declaration's own template parameters (the pattern
	// form that registers on the owner template).
	bool TemplateMemberOwnerIsPattern(const AstDecl& decl,
	                                  const AstName& name);
	bool ExprMentionsAny(const AstExpr& expr,
	                     const std::set<string>& params);

	// --- PA17 virtual members (sem_virtual.cpp) ---
	// Declaration-time slot recording for an ordinary member function
	// (10.3p2 override matching, 10.3p4 final, 10.3p7 covariant returns)
	// and for a declared destructor.
	void RecordVirtualMember(ClassInfo& cls, const string& name,
	                         const TypePtr& type,
	                         const DeclSpecifierInfo* specs,
	                         const DeclaratorInfo* composed, bool pure,
	                         bool defined);
	void RecordVirtualDtor(ClassInfo& cls, bool declared_virtual,
	                       const DeclaratorInfo& composed, bool defined,
	                       bool defaulted, bool deleted);
	// Class-completion fixup: an implicit destructor overrides an
	// inherited virtual destructor (12.4p9).
	void FinishClassVirtualFacts(ClassInfo& cls);
	// The vpointer-store action of a constructor/destructor body.
	SemNodePtr MakeVPointerStore(const ClassInfo& cls);
	bool NodeMayThrow(const SemNode& node) const;
	bool DerivedToBaseClass(const TypePtr& from, const TypePtr& to);
	void AttachObjectLifetime(SemNode& item, ScopeBinding& binding,
	                          const AstInitializer* init,
	                          const DeclSpecifierInfo& specs);
	void AppendClassObjectInit(SemNode& item, ScopeBinding& binding,
	                           const AstInitializer* init,
	                           const ClassInfo& cls);
	bool ClassifyClassInitForm(const AstInitializer& init,
	                           vector<const AstExpr*>& args,
	                           const AstExpr*& braced);
	void AppendClassArrayInit(SemNode& item, ScopeBinding& binding,
	                          const AstInitializer* init,
	                          const ClassInfo& cls);
	void AppendArrayElementInit(SemNode& item, ScopeBinding& binding,
	                            const ClassInfo& cls,
	                            unsigned long long at,
	                            const AstExpr* element);
	void AppendElidedObjectInit(SemNode& item, ScopeBinding& binding,
	                            const ClassInfo& cls, SemNodePtr action);
	void AppendAggregateInit(const ClassInfo& cls,
	                         const SemNode& target_proto,
	                         const AstExpr& braced,
	                         vector<SemNodePtr>& out);
	size_t ConsumeAggregateItems(const ClassInfo& cls,
	                             const SemNode& target_proto,
	                             const vector<AstExprPtr>& items,
	                             size_t at, bool top_braced,
	                             vector<SemNodePtr>& out);
	size_t ConsumeAggregateClassItem(const ClassInfo& member_cls,
	                                 const ClassField& field,
	                                 SemNodePtr member,
	                                 const vector<AstExprPtr>& items,
	                                 size_t at, vector<SemNodePtr>& out);
	// A work-free member-class temporary retargeted onto the member
	// (the pinned copy-elision shape).
	SemNodePtr ElideMemberTemporary(const ClassInfo& member_cls,
	                                SemNodePtr action, SemNodePtr member);
	size_t ConsumeArrayItems(const ClassField& field,
	                         const SemNode& member_proto,
	                         const vector<AstExprPtr>& items,
	                         size_t at, bool top_braced,
	                         vector<SemNodePtr>& out);
	// The synthesized field-wise constructor used by aggregate array
	// elements; returns its this-adjusted type.
	TypePtr EnsureAggregateCtor(const ClassInfo& cls);
	void AppendAggregateArrayInit(SemNode& item, ScopeBinding& binding,
	                              const ClassInfo& cls,
	                              const AstExpr& braced);
	SemNodePtr VariableObjectExpr(const ScopeBinding& binding);
	// The set of bit-field storage units already written by the open
	// constructor synthesis (first writes store plainly).
	std::map<unsigned long long, bool> bf_units_written_;
	bool TryVexingCallRecovery(const AstDecl& decl);
	// Call expressions synthesized by the statement disambiguation
	// recovery (owned here; analyzed nodes reference them).
	vector<AstExprPtr> recovered_exprs_;

	SemUnit& unit_;
	SemExprAnalyzer analyzer_;
	// PA20 constant engine: evaluated constexpr objects and function
	// bodies over the analyzed trees.
	ConstEvalEngine engine_;
	// The analyzed in-class initialization actions of object-valued
	// static members, keyed like the constant store; the member's
	// storage definition hands them to the lowering for odr-use
	// tracking (SemUnit::const_image_inits).
	std::map<std::pair<const void*, string>, SemNodePtr>
		member_image_inits_;
	// The open container chain; items append to the innermost node (or
	// the unit when empty).
	vector<SemNode*> parents_;
	// Classes whose implicit default constructor was synthesized, with
	// their qualified constructor names.
	std::map<const NamedTypeInfo*, string> constructors_;
	TypePtr current_return_;  // return type of the open function body
	int local_types_;
	bool pending_local_type_;

	// --- PA15 class state (sem_class.cpp) ---
	// One queued in-class member-function (or hidden-friend) body,
	// analyzed when the outermost enclosing class completes (9.2p2).
	struct DeferredBody
	{
		DeferredBody() : decl(0), fn_scope(0), declaring(0), cls(0),
		                 is_friend(false), is_static(false),
		                 out_of_class(false) {}

		const AstDecl* decl;
		DeclaratorInfo composed;  // DK_FUNCTION methods / friends
		string name;
		Scope* fn_scope;
		Scope* declaring;   // class scope (methods) / namespace (friends)
		ClassInfo* cls;     // lexical class context
		bool is_friend;
		bool is_static;
		bool out_of_class;  // qualified definition: strong emission
	};
	// The current function context while a body is analyzed: the member
	// class (methods), the lexical class (hidden friends), and the
	// function's own identity for friendship checks.
	struct MethodContext
	{
		MethodContext() : cls(0), lexical_cls(0), fn_scope(0), fn_owner(0)
		{}

		const ClassInfo* cls;          // null outside member functions
		const ClassInfo* lexical_cls;  // hidden friend's lexical class
		TypePtr this_type;  // pointer to cv class (null outside methods)
		Scope* fn_scope;
		const Scope* fn_owner;  // declaring scope of the open function
		string fn_name;
		// PA21: the template name behind an instantiated specialization
		// body ("read" while binding read<int>); friend grants recorded
		// under the template name match through it.
		string fn_template_name;
	};

	vector<ClassInfo*> open_classes_;
	vector<DeferredBody> deferred_bodies_;
	// PA21: poisoned instantiated bodies retried at the end of the
	// unit (the poisoned deferred item is replaced on success).
	struct RetryBody
	{
		RetryBody() : deferred_index(0) {}

		DeferredBody body;
		size_t deferred_index;
	};
	vector<RetryBody> retry_bodies_;
	MethodContext method_;
	// Saved method contexts around member-signature composition.
	vector<MethodContext> signature_contexts_;
	bool in_bit_field_;

	// --- PA18 template state (sem_template.cpp / template_deduce.cpp) ---
	// True while binding instantiated declarations: their definitions
	// emit weak (demand-emitted) instead of strong.
	bool instantiating_;
	int instantiation_depth_;
	// Shared positional deduction placeholders (`#0`, `#1`, ...).
	vector<TypePtr> placeholders_;
	// Synthesized unique argument types for partial ordering.
	vector<TypePtr> ordering_uniques_;
	// True inside contexts that are grammatically type-only (base
	// clauses): 14.6p3 typename is not required there.
	bool in_implicit_type_context_;
	// True while analyzing an unevaluated operand (decltype, constant
	// sizeof): names there are not odr-used (3.2p2), so specialization
	// bodies do not instantiate.
	bool in_unevaluated_operand_;
	// Non-null while composing a function-template signature: composed
	// parameters bind here so trailing-return decltype resolves them.
	Scope* param_capture_scope_;
	// Deferred member-class definitions of instantiated classes
	// (14.7.1p1), completed on demand by EnsureTypeCompleteness.
	struct PendingClassDefinition
	{
		PendingClassDefinition() : decl(0), scope(0) {}

		const AstDecl* decl;
		Scope* scope;  // the class scope the definition binds in
	};
	std::map<const NamedTypeInfo*, PendingClassDefinition> pending_classes_;

public:
	// IConstExprContext (const_expr.h): the analyzer classifies the
	// unevaluated sizeof operand (5.3.3p1).
	virtual TypePtr TryAnalyzeExpressionType(const AstExpr& expr);
	// PA19 restricted constexpr conversion (`B{}` as a constant):
	// template_args.cpp.
	virtual bool TryClassConversionConstant(const AstName& name,
	                                        ConstValue& out);
	// PA21 sizeof...(pack) in constant expressions (sem_pack.cpp).
	virtual bool PackSizeConstant(const string& name,
	                              unsigned long long& out);
	// ISemExprHost template hooks.
	virtual const FunctionSpecialization* DeduceFunctionTemplate(
		TemplateInfo& tmpl, const vector<SemValue>& args,
		const AstNamePart* explicit_part);
	virtual Scope* SwapLookupScope(Scope* scope);
	virtual void RequireCompleteType(const NamedTypeInfo* info);
	// ITypeBuilderHost: arrays of abstract classes are ill-formed
	// (10.4p3); substitution contexts SFINAE-discard the throw.
	virtual void CheckArrayElementType(const TypePtr& element);
	// Deduces the source class's conversion-function templates (base
	// chain included) against the required destination type; each
	// success synthesizes an ordinary ClassConversion entry
	// (template_deduce.cpp, reached through the sem_convert hook).
	void DeduceConversionTemplates(const NamedTypeInfo* entity,
	                               const TypePtr& dest);
	// Deduces the destination class's constructor templates against a
	// conversion source, synthesizing ClassCtor entries for the
	// converting-constructor classification.
	void DeduceCtorTemplatesForConversion(const NamedTypeInfo* entity,
	                                      const ConversionSource& source);
	// PA18 14.5.6.2 subset (template_deduce.cpp): partial ordering of
	// two deduced candidates over the call's leading `argc` parameters.
	virtual bool TemplateCandidateMoreSpecialized(
		const FunctionSpecialization* a,
		const FunctionSpecialization* b, size_t argc);
	TypePtr OrderingUniqueType(size_t index);
	bool OrderingAtLeastAsSpecialized(TemplateInfo& a, TemplateInfo& b,
	                                  size_t argc);
	virtual const FunctionSpecialization* DeduceFunctionTemplateFromTarget(
		TemplateInfo& tmpl, const TypePtr& target);
	virtual void OnSpecializationOdrUsed(const FunctionSpecialization* spec);
	virtual void OnStaticMemberReferenced(const ScopeBinding& binding);
	virtual bool IsInstantiating() const { return instantiating_; }
	virtual const FunctionSpecialization* InstantiateCharPackLiteral(
		const ScopeBinding& binding, const string& chars);
	virtual bool SwapUnevaluatedOperand(bool active);
	virtual void OnMemberSignatureBegin(Scope* class_scope);
	virtual void OnMemberSignatureEnd();
	virtual void OnParameterComposed(const string& name,
	                                 const TypePtr& type);
};

// Saved-and-cleared binder state around one instantiation: the
// instantiated declarations bind in their own context (the template's
// lexical scope), never into the open class/function/dump state of the
// use site. `instantiating` marks a body/definition instantiation
// (weak emission); signature composition passes false and inherits
// the surrounding mode. The destructor restores everything
// (exception-safe); bodies live in sem_template.cpp.
struct SemBinder::InstantiationContext
{
	InstantiationContext(SemBinder& binder, Scope* scope,
	                     bool instantiating = false);
	~InstantiationContext();

private:
	SemBinder& binder_;
	Scope* saved_scope_;
	std::vector<TypePtr>* saved_fields_;
	EMemberAccess saved_access_;
	bool saved_c_linkage_;
	MethodContext saved_method_;
	TypePtr saved_return_;
	bool saved_bit_field_;
	bool saved_instantiating_;
	bool saved_unevaluated_;
	Scope* saved_param_capture_;
	std::map<unsigned long long, bool> saved_bf_units_;
	vector<SemNode*> saved_parents_;
	vector<ClassInfo*> saved_open_classes_;
	vector<DeferredBody> saved_deferred_;
};

