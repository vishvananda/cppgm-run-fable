#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "post_token.h"

// The PA10 syntax tree, part 1: structured names, specifier
// sequences, type-ids, declarators, parameters, and initializers.
// Nodes are kind-tagged structs with typed fields so the PA11/PA12
// semantic passes can classify them without reparsing source text.

struct AstExpr;
struct AstTypeId;
struct AstDecl;
struct AstStmt;
struct AstDeclarator;
struct AstInitializer;
struct AstLambda;
struct AstParameterClause;

typedef std::unique_ptr<AstExpr> AstExprPtr;
typedef std::unique_ptr<AstTypeId> AstTypeIdPtr;
typedef std::unique_ptr<AstDecl> AstDeclPtr;
typedef std::unique_ptr<AstStmt> AstStmtPtr;
typedef std::unique_ptr<AstDeclarator> AstDeclaratorPtr;
typedef std::unique_ptr<AstInitializer> AstInitializerPtr;
typedef std::unique_ptr<AstLambda> AstLambdaPtr;
typedef std::unique_ptr<AstParameterClause> AstParameterClausePtr;

// --- names -----------------------------------------------------------

enum ENamePartKind
{
	NP_IDENTIFIER,           // x (tilde flag for ~x)
	NP_TEMPLATE_ID,          // x<args> (tilde flag for ~x<args>)
	NP_OPERATOR_FUNCTION,    // operator @ (operator_text holds "@")
	NP_CONVERSION_FUNCTION,  // operator type-id
	NP_LITERAL_OPERATOR,     // operator "" x
	NP_DECLTYPE              // decltype ( expression )
};

// One template-argument: a type-id or an expression, optionally a pack
// expansion. The choice may be syntactic only; later assignments
// re-classify.
struct AstTemplateArgument
{
	AstTemplateArgument();

	bool is_type;
	AstTypeIdPtr type;
	AstExprPtr expr;
	bool pack;
};

struct AstNamePart
{
	AstNamePart();

	ENamePartKind kind;
	std::string identifier;    // identifier / template-name / ud-suffix
	bool tilde;                // destructor form ~identifier
	bool template_keyword;     // preceded by the `template` keyword
	// NP_OPERATOR_FUNCTION with a parsed template-argument clause
	// (`operator+<>`): the empty clause leaves `arguments` empty, so
	// the template-id form needs its own mark.
	bool operator_template_id = false;
	std::vector<AstTemplateArgument> arguments;  // NP_TEMPLATE_ID
	std::string operator_text;                   // NP_OPERATOR_FUNCTION
	AstTypeIdPtr conversion_type;                // NP_CONVERSION_FUNCTION
	AstExprPtr decltype_expr;                    // NP_DECLTYPE
};

// A possibly qualified name: nested-name-specifier parts followed by
// the terminal unqualified-id, with optional leading :: and typename.
struct AstName
{
	AstName();

	bool IsPlainIdentifier() const;

	bool global_scope;
	bool typename_keyword;
	std::vector<AstNamePart> parts;
};

// --- specifiers and type-ids -----------------------------------------

enum ESpecifierKind
{
	SPEC_KEYWORD,    // simple type, cv, storage, function specifiers
	SPEC_TYPE_NAME,  // qualified-type-name / typename-specifier
	SPEC_DECLTYPE,   // decltype-specifier
	SPEC_NESTED_DECL,// class-specifier / enum-specifier /
	                 // class-forward-declaration used as a specifier
	SPEC_TRANSFORM,  // PA33 builtin transform: __decay ( type-id )
	SPEC_BITINT,     // PA34 _BitInt ( constant-expression )
	SPEC_COMPLEX     // PA34 GNU _Complex/__complex__ qualification
};

struct AstSpecifier
{
	AstSpecifier();

	bool IsCv() const;

	ESpecifierKind kind;
	ETokenType keyword;        // SPEC_KEYWORD
	std::string spelling;      // SPEC_KEYWORD source spelling;
	                           // SPEC_TRANSFORM trait name
	AstName name;              // SPEC_TYPE_NAME
	AstExprPtr decltype_expr;  // SPEC_DECLTYPE; SPEC_BITINT width
	AstDeclPtr nested_decl;    // SPEC_NESTED_DECL
	AstTypeIdPtr transform_type;  // SPEC_TRANSFORM operand;
	                              // GNU typeof(type-id) operand
	// GNU __typeof__/typeof: decltype semantics with references
	// stripped (the GNU operator never yields a reference type).
	bool typeof_strip = false;
};

typedef std::vector<AstSpecifier> AstSpecifierSeq;

struct AstTypeId
{
	AstSpecifierSeq specifiers;
	AstDeclaratorPtr declarator;  // abstract; null or empty when absent
};

// --- declarators ------------------------------------------------------

enum EDeclaratorItemKind
{
	DI_PTR,              // * & &&
	DI_MEMBER_PTR,       // nested-name-specifier ::*
	DI_CV,               // cv-qualifier (after a pointer or a clause)
	DI_PACK,             // ... pack marker
	DI_ID,               // declarator-id
	DI_NESTED,           // ( declarator )
	DI_PARAMS,           // parameter clause
	DI_FUNC_QUAL,        // noexcept / throw() / virt-specifier
	DI_TRAILING_RETURN,  // -> type-id
	DI_ARRAY             // [ expression? ]
};

enum EFuncQualKind
{
	FQ_NOEXCEPT,       // noexcept, optional ( expression )
	FQ_THROW,          // throw ( type-id-list? )
	FQ_VIRT            // override / final / ref-qualifier
};

struct AstFunctionQualifier
{
	AstFunctionQualifier();

	EFuncQualKind kind;
	bool has_expr;
	AstExprPtr expr;
	std::vector<AstTypeIdPtr> throw_types;
	std::string spelling;  // FQ_VIRT
};

struct AstDeclaratorItem
{
	AstDeclaratorItem();

	EDeclaratorItemKind kind;
	ETokenType token;          // DI_PTR, DI_CV
	std::string spelling;      // DI_PTR, DI_CV source spelling
	AstName name;              // DI_ID, DI_MEMBER_PTR (the qualifier)
	AstDeclaratorPtr nested;   // DI_NESTED
	AstParameterClausePtr params;       // DI_PARAMS
	AstFunctionQualifier qual;          // DI_FUNC_QUAL
	AstTypeIdPtr trailing_type;         // DI_TRAILING_RETURN
	AstExprPtr array_bound;             // DI_ARRAY (may be null)
};

// Flat source-ordered declarator item list; the named/abstract
// distinction is whether a DI_ID item is present.
struct AstDeclarator
{
	bool Empty() const { return items.empty(); }
	const AstName* IdName() const;  // declarator-id, through nesting

	std::vector<AstDeclaratorItem> items;
	// PA33 __attribute__((abi_tag("..."))) tags captured after the
	// declarator (they append B<len><tag> to the mangled name).
	std::vector<std::string> abi_tags;
	// PA36 GNU asm label (`declarator __asm("name")`): the declared
	// entity's object symbol is this exact name.
	std::string asm_label;
};

struct AstParameter
{
	AstSpecifierSeq specifiers;
	AstDeclaratorPtr declarator;     // null when absent
	AstInitializerPtr default_arg;   // null when absent
};

struct AstParameterClause
{
	AstParameterClause();

	std::vector<AstParameter> parameters;
	bool variadic;  // trailing , ... or lone ...
};

// --- initializers ----------------------------------------------------

enum EInitializerKind
{
	INIT_EQ,      // = initializer-clause
	INIT_PAREN,   // ( initializer-clause-list? )
	INIT_BRACED,  // braced-init-list
	INIT_DEFAULT, // = default
	INIT_DELETE   // = delete
};

struct AstInitializer
{
	AstInitializer();

	EInitializerKind kind;
	AstExprPtr expr;               // INIT_EQ, INIT_BRACED (braced expr)
	std::vector<AstExprPtr> args;  // INIT_PAREN
};
