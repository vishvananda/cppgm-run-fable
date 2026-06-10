#pragma once

#include <map>

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
class DeclBinder : public ITypeBuilderHost, public IConstExprContext
{
public:
	explicit DeclBinder(TypesModel& model);

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

private:
	// Redeclaration facts of one enumeration entity (7.2p2).
	struct EnumFacts
	{
		EnumFacts() : scoped(false), defined(false) {}
		TypePtr underlying;
		bool scoped;
		bool defined;
	};

	// --- declarations ---
	void BindDeclarations(const std::vector<AstDeclPtr>& decls);
	void BindDeclaration(const AstDecl& decl);
	void BindNamespace(const AstDecl& decl);
	void BindNamespaceAlias(const AstDecl& decl);
	void BindUsingDirective(const AstDecl& decl);
	void BindUsingDeclaration(const AstDecl& decl);
	void BindStaticAssert(const AstDecl& decl);
	void BindSimpleDeclaration(const AstDecl& decl);
	void BindInitDeclarator(const DeclSpecifierInfo& specs,
	                        const AstInitDeclarator& declarator);
	void BindTypeAlias(const string& name, const TypePtr& type);
	void BindVariable(const string& name, const TypePtr& type,
	                  const AstInitializer* init, bool is_static);
	void RecordConstantValue(ScopeBinding& binding,
	                         const AstInitializer* init);
	void BindFunctionDefinition(const AstDecl& decl);
	void BindTemplateDeclaration(const AstDecl& decl);
	void BindBitFieldDeclaration(const AstDecl& decl);

	// --- classes and enums ---
	TypePtr BindClass(const AstDecl& decl, bool standalone);
	TypePtr BindClassForward(const AstDecl& decl, bool elaborated);
	void CompleteClassLayout(NamedTypeInfo& info,
	                         const std::vector<TypePtr>& fields);
	void InjectAnonymousUnionMembers(const Scope& union_scope);
	TypePtr BindEnum(const AstDecl& decl);
	TypePtr DeclareEnumEntity(const AstDecl& decl, const string& name,
	                          bool scoped, const TypePtr& underlying);
	void BindEnumerators(const AstDecl& decl, const TypePtr& enum_type);

	// --- statements (block scopes and local declarations only) ---
	void BindStatement(const AstStmt& stmt);

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

	string AnonymousTypeName(const AstDecl& decl) const;

	TypesModel& model_;
	TypeBuilder builder_;
	Scope* current_;
	// Non-static data member types of the class currently being bound
	// (null outside a class-specifier body), for layout completion.
	std::vector<TypePtr>* current_fields_;
	std::map<const NamedTypeInfo*, EnumFacts> enum_facts_;
	int anonymous_enums_;
};
