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

	void BindTranslationUnit(const AstDecl& unit);

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
	void BindTemplateDeclaration(const AstDecl& decl);
	void BindBitFieldDeclaration(const AstDecl& decl);

	// --- declaration events (PA12 dump recording; no-ops in PA11) ---
	virtual void OnTypeAliasBound(const string& name, const TypePtr& type);
	virtual void OnVariableBound(ScopeBinding& binding,
	                             const AstInitializer* init,
	                             const DeclSpecifierInfo& specs);
	virtual void OnFunctionDeclared(ScopeBinding& binding,
	                                const TypePtr& type);

	// --- classes and enums ---
	TypePtr BindClass(const AstDecl& decl, bool standalone);
	TypePtr BindClassForward(const AstDecl& decl, bool elaborated);
	void CompleteClassLayout(NamedTypeInfo& info,
	                         const std::vector<TypePtr>& fields);
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
	static const string& TerminalName(const AstName& name);
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
	// Non-static data member types of the class currently being bound
	// (null outside a class-specifier body), for layout completion.
	std::vector<TypePtr>* current_fields_;
	int anonymous_enums_;
};
