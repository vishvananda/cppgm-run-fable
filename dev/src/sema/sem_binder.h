#pragma once

#include <map>

#include "sema/class_info.h"
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

	// ISemExprHost
	virtual Scope* CurrentScope();
	virtual TypesModel& Model();
	virtual const ScopeBinding* ResolveValue(const AstName& name,
	                                         const NamedTypeInfo*& member_class);
	virtual TypePtr TryResolveCalleeType(const AstName& name);
	virtual TypePtr ResolveCastTypeId(const AstTypeId& type_id);
	virtual bool TryEvaluateConstant(const AstExpr& expr, ConstValue& value);
	virtual const ScopeBinding* ResolveBuiltinFunction(const string& name);
	virtual ClassRegistry& Classes();
	virtual const ClassInfo* CurrentClass();
	virtual TypePtr CurrentThisType();
	virtual void CheckMemberAccess(const Scope* owner, EMemberAccess access,
	                               const string& what);
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
	                                const TypePtr& type);
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
	void BindFriendFunction(const AstDecl& decl, ClassInfo& cls);
	// PA16: a conversion-function member declaration (12.3.2).
	void BindConversionFunction(const AstDecl& decl, ClassInfo& cls,
	                            const AstNamePart& part);
	void BindMemberFunctionBody(const AstDecl& decl,
	                            const DeclaratorInfo& composed,
	                            const string& name);
	void FlushDeferredBodies();
	void AnalyzeDeferredBody(const DeferredBody& body);
	void BindQualifiedSpecialMember(const AstDecl& decl,
	                                const AstName& id);
	Scope* MakeSpecialMemberScope(const string& name,
	                              const DeclaratorInfo& composed,
	                              ClassInfo& cls);
	TypePtr MethodAdjustedType(const ClassInfo& cls, const TypePtr& member);
	SemNodePtr BuildFunctionNode(const DeferredBody& body,
	                             ESpecialFunction special);
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
	                                        bool is_move,
	                                        unsigned long long& alignment,
	                                        size_t& first_suffix);
	void AppendTransferActions(const ClassInfo& cls, bool is_move,
	                           bool assign_form, const SemNode& source_proto,
	                           vector<SemNodePtr>& out);
	virtual void BindInheritingConstructors(Scope* base_scope);
	SemNodePtr MakeDestructorCall(const ClassInfo& cls, bool base_entry,
	                              SemNodePtr address);
	bool NodeMayThrow(const SemNode& node) const;
	bool DerivedToBaseClass(const TypePtr& from, const TypePtr& to);
	void AttachObjectLifetime(SemNode& item, ScopeBinding& binding,
	                          const AstInitializer* init,
	                          const DeclSpecifierInfo& specs);
	void AppendClassObjectInit(SemNode& item, ScopeBinding& binding,
	                           const AstInitializer* init,
	                           const ClassInfo& cls);
	void AppendClassArrayInit(SemNode& item, ScopeBinding& binding,
	                          const AstInitializer* init,
	                          const ClassInfo& cls);
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
	};

	vector<ClassInfo*> open_classes_;
	vector<DeferredBody> deferred_bodies_;
	MethodContext method_;
	bool in_bit_field_;
};
