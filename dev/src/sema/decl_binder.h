#pragma once

#include "ast/ast.h"
#include "sema/const_expr.h"
#include "sema/scope.h"
#include "sema/scope_lookup.h"
#include "sema/type_builder.h"

// PA11 declaration binding: one forward traversal of a translation
// unit's PA10 AST that builds the TypesModel scope tree - scope
// formation, declaration collection, redeclaration merging, and the
// constant evaluation the declarations need (array bounds, enumerator
// values, static_assert). The builder and evaluator call back through
// the two interfaces; everything outside the PA11 assignment boundary
// throws, which the driver reports as EXIT_FAILURE.
//
// The traversal exposes protected virtual seams (display naming,
// function-name binding, namespace bodies, declaration events,
// statement binding) so the PA12 semantic binder can extend the same
// forward pass with overload sets, expression analysis, and dump
// recording without duplicating the declaration machinery.
class DeclBinder : public ITypeBuilderHost, public IConstExprContext
{
public:
	explicit DeclBinder(TypesModel& model);
	virtual ~DeclBinder() {}

	virtual void BindTranslationUnit(const AstDecl& unit);

	// ITypeBuilderHost
	virtual TypePtr BindNestedTypeSpecifier(const AstDecl& decl);
	virtual TypePtr ResolveTypeName(const AstName& name);
	virtual TypePtr ResolveDecltype(const AstExpr& expr);
	virtual unsigned long long EvaluateArrayBound(const AstExpr& expr);

	// IConstExprContext
	virtual ConstValue LookupConstant(const AstName& name);
	virtual TypePtr TryResolveTypeFromName(const AstName& name);
	virtual TypePtr ResolveTypeId(const AstTypeId& type_id);

protected:
	// --- declarations ---
	void BindDeclarations(const std::vector<AstDeclPtr>& decls);
	void BindDeclaration(const AstDecl& decl);
	void BindNamespace(const AstDecl& decl);
	// The body traversal of a (possibly reopened) namespace-definition;
	// PA12 wraps it to record the dump node.
	virtual void BindNamespaceBody(const AstDecl& decl, Scope* scope);
	void BindNamespaceAlias(const AstDecl& decl);
	void BindUsingDirective(const AstDecl& decl);
	void BindUsingDeclaration(const AstDecl& decl);
	// PA15: `using Base::Base` (12.9); PA11 rejects it.
	virtual void BindInheritingConstructors(Scope* base_scope);
	void MergeImportedOverloads(ScopeBinding& own,
	                            const ScopeBinding& imported);
	void BindStaticAssert(const AstDecl& decl);
	virtual void BindSimpleDeclaration(const AstDecl& decl);
	void BindInitDeclarator(const DeclSpecifierInfo& specs,
	                        const AstInitDeclarator& declarator);
	void BindTypeAlias(const string& name, const TypePtr& type);
	void BindVariable(const string& name, const TypePtr& type,
	                  const AstInitializer* init,
	                  const DeclSpecifierInfo& specs);
	void RecordConstantValue(ScopeBinding& binding,
	                         const AstInitializer* init);
	void BindFunctionDefinition(const AstDecl& decl);
	// Find-or-extend the binding of a function name declared in the
	// current scope (PA11: a different type is an error; PA12 collects
	// overloads). `allow_block` admits block-scope redeclarations.
	virtual ScopeBinding& BindFunctionName(const string& name,
	                                       const TypePtr& type,
	                                       bool allow_block);
	// The body of a function definition; called with `current_` set to
	// the new function scope (parameters bound). PA11 only walks block
	// scopes and local declarations; PA12 analyzes the statements.
	virtual void BindFunctionBody(const AstDecl& decl,
	                              const DeclaratorInfo& composed,
	                              const string& name);
	// PA11 binds the parameter scope and the inner declaration for the
	// types dump; the PA18 binder overrides this to capture the
	// template for on-demand instantiation instead.
	virtual void BindTemplateDeclaration(const AstDecl& decl);
	// PA18 seams: explicit instantiation and template-id name parts
	// (`Foo<int>` as a type name or nested-name-specifier component)
	// stay outside the PA11 boundary.
	virtual void BindExplicitInstantiation(const AstDecl& decl);
	// The binding a template-id name part designates: `prefix` is the
	// scope its nested-name-specifier resolved to (null for an
	// unqualified template-id). PA18 instantiates the named class
	// template on demand.
	virtual const ScopeBinding* ResolveTemplateIdBinding(
		const AstNamePart& part, Scope* prefix);

	// --- declaration events (PA12 dump recording; no-ops in PA11) ---
	virtual void OnTypeAliasBound(const string& name, const TypePtr& type);
	virtual void OnVariableBound(ScopeBinding& binding,
	                             const AstInitializer* init,
	                             const DeclSpecifierInfo& specs);
	// PA17: member declarations pass the specifier/declarator facts and
	// the pure-specifier through so the binder can record virtual-slot
	// metadata (null/false from contexts that carry none).
	virtual void OnFunctionDeclared(ScopeBinding& binding,
	                                const TypePtr& type,
	                                const DeclSpecifierInfo* specs = 0,
	                                const DeclaratorInfo* composed = 0,
	                                bool pure = false);

	// --- classes and enums ---
	// The DK_CLASS dispatch seam: PA18 defers nested member-class
	// definitions inside instantiations and admits qualified
	// out-of-class nested-class definitions.
	virtual void BindClassDeclaration(const AstDecl& decl);
	// Completes a deferred (pending) class definition when a context
	// requires the complete type; no-op in PA11.
	virtual void EnsureTypeCompleteness(const NamedTypeInfo* info);
	TypePtr BindClass(const AstDecl& decl, bool standalone);
	TypePtr BindClassForward(const AstDecl& decl, bool elaborated);
	void CompleteClassLayout(NamedTypeInfo& info,
	                         const std::vector<TypePtr>& fields,
	                         unsigned long long min_alignment);
	// The strictest class-head alignas value (7.6.2), zero when the
	// declaration carries no alignment-specifier.
	unsigned long long RequestedAlignment(const AstDecl& decl);
	// PA15 seams: base-clause binding (PA11 rejects bases), class
	// open/close events, and completion. The default completion runs
	// the PA11 field-sequential layout.
	virtual void BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
	                            Scope* scope);
	virtual void OnClassOpened(const AstDecl& decl, NamedTypeInfo* info,
	                           Scope* scope);
	virtual void CompleteClass(const AstDecl& decl, NamedTypeInfo* info,
	                           Scope* scope,
	                           const std::vector<TypePtr>& fields);
	// PA15: constructors and destructors (PA11 skips them silently).
	virtual void BindSpecialMember(const AstDecl& decl);
	// PA15 reimplements bit-field binding with widths and layout.
	virtual void BindBitFieldDeclaration(const AstDecl& decl);
	// PA15: friend declarations and hidden-friend definitions (11.3).
	// PA11 rejects every friend form.
	static bool HasFriendSpecifier(const AstDecl& decl);
	virtual void BindFriendDeclaration(const AstDecl& decl);
	// PA15 admits class scopes for qualified function definitions.
	virtual void CheckQualifiedDefinitionScope(const Scope* declaring);
	// PA15: qualified non-function declarators (static data member
	// definitions); PA11 rejects them.
	virtual void BindQualifiedDeclarator(const DeclSpecifierInfo& specs,
	                                     const AstInitDeclarator& declarator,
	                                     const DeclaratorInfo& composed);
	// 9.5p5 member injection of a standalone anonymous union; PA12 also
	// synthesizes the storage variable and its construction.
	virtual void BindAnonymousUnionMembers(const AstDecl& decl,
	                                       const TypePtr& type,
	                                       const Scope& union_scope);
	TypePtr BindEnum(const AstDecl& decl);
	TypePtr DeclareEnumEntity(const AstDecl& decl, const string& name,
	                          bool scoped, const TypePtr& underlying);
	void BindEnumerators(const AstDecl& decl, const TypePtr& enum_type);

	// --- statements (block scopes and local declarations only) ---
	virtual void BindStatement(const AstStmt& stmt);

	// --- names ---
	// Validates the PA11-supported name shape (plain identifier parts).
	static const string& PartName(const AstNamePart& part);
	// PA16: terminal parts include operator-function-ids ("operator =").
	static string TerminalName(const AstName& name);
	// PA14: the declared name of a function declarator-id terminal,
	// accepting operator-function-ids ("operator delete").
	static string DeclaredFunctionName(const AstNamePart& part);
	// PA14: records per-overload facts (default arguments, deleted
	// definitions) onto the function binding for the overload whose
	// parameter list matches `composed.type`. PA15 adds the member
	// facts: access, static-ness, in-class definition, friend-only
	// visibility, and simple-noexcept marking.
	void RecordFunctionFacts(ScopeBinding& binding,
	                         const DeclaratorInfo& composed, bool deleted,
	                         const DeclSpecifierInfo* specs = 0,
	                         bool inline_def = false, bool adl_only = false);
	// The scope a nested-name-specifier component designates.
	Scope* ScopeOfBinding(const ScopeBinding& binding);
	// The scope of name parts [0, parts.size()-1).
	Scope* ResolvePrefixScope(const AstName& name);
	// Full resolution of a possibly qualified name (null: not found).
	const ScopeBinding* ResolveTerminal(const AstName& name,
	                                    EScopeLookupFilter filter);
	// The namespace a using-directive or namespace-alias target names.
	Scope* ResolveNamespaceTarget(const AstName& name);
	// The declared type of an id-expression's entity (decltype forms).
	TypePtr DeclaredEntityType(const AstName& name,
	                           bool& lvalue_entity);

	// The canonical display spelling of a named type ("struct C"); the
	// PA12 binder qualifies it with the enclosing namespace path.
	virtual string TypeDisplayName(const string& key,
	                               const string& name) const;
	virtual string AnonymousTypeName(const AstDecl& decl);

	TypesModel& model_;
	TypeBuilder builder_;
	Scope* current_;
	// PA18: a qualified class-name on a class definition is admitted
	// when the caller (the PA18 seam) resolved the prefix scope.
	bool allow_qualified_class_name_;
	// Non-static data member types of the class currently being bound
	// (null outside a class-specifier body), for layout completion.
	std::vector<TypePtr>* current_fields_;
	int anonymous_enums_;
	// PA14: inside an extern "C" linkage-specification body.
	bool in_c_linkage_;
	// PA15: the access of the current member-specification region
	// (11p1: private for `class`, public for `struct`/`union`).
	EMemberAccess current_access_;
};
