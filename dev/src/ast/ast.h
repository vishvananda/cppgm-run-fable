#pragma once

#include "ast/ast_expr.h"

// The PA10 syntax tree, part 3: declarations. The deterministic dump
// (ast_printer) and the one-line annotation spellings (ast_text) are
// renderings of this typed structure.

enum EDeclKind
{
	DK_TRANSLATION_UNIT,
	DK_EMPTY,
	DK_SIMPLE,
	DK_FUNCTION,
	DK_NAMESPACE,
	DK_NAMESPACE_ALIAS,
	DK_USING_DIRECTIVE,
	DK_USING_DECLARATION,
	DK_ALIAS,
	DK_LINKAGE,
	DK_TEMPLATE,
	DK_CLASS,           // class-specifier with a body, used standalone
	DK_CLASS_FORWARD,   // class-key name ; and elaborated specifiers
	DK_ENUM,            // enum-specifier, standalone or as a specifier
	DK_STATIC_ASSERT,
	DK_ACCESS_LABEL,
	DK_BIT_FIELD,
	DK_SPECIAL_MEMBER_DECLARATION,
	DK_SPECIAL_MEMBER_DEFINITION,
	DK_EXPLICIT_INSTANTIATION
};

struct AstInitDeclarator
{
	AstDeclaratorPtr declarator;
	AstInitializerPtr init;  // null when absent
};

struct AstEnumerator
{
	std::string name;
	AstExprPtr value;  // null when absent
};

struct AstBaseSpecifier
{
	AstBaseSpecifier();

	bool is_virtual;
	std::string virtual_spelling;
	bool has_access;
	ETokenType access;
	std::string access_spelling;
	AstName name;  // base-name (decltype form uses NP_DECLTYPE)
	bool pack;
};

enum ETemplateParameterKind
{
	TP_TYPE,      // class / typename parameter
	TP_TEMPLATE,  // template-template parameter
	TP_NON_TYPE   // declarator-style parameter
};

struct AstTemplateParameter
{
	AstTemplateParameter();

	ETemplateParameterKind kind;
	ETokenType key;            // class / typename
	std::string key_spelling;
	bool pack;
	std::string name;          // may be empty
	std::vector<AstTemplateParameter> template_params;  // TP_TEMPLATE
	bool has_default_type;
	AstTypeIdPtr default_type;
	AstSpecifierSeq specifiers;       // TP_NON_TYPE
	AstDeclaratorPtr declarator;      // TP_NON_TYPE, may be null
	bool has_default_expr;
	AstExprPtr default_expr;
	// Reference-format quirk: a declarator-less keyword-typed non-type
	// parameter dumps its literal default as a token leaf.
	bool default_token_form;
};

struct AstMemInitializer
{
	AstName id;
	AstInitializerPtr init;  // INIT_PAREN or INIT_BRACED
};

struct AstMemberSpecifier
{
	AstMemberSpecifier();

	ETokenType keyword;
	std::string spelling;
};

struct AstDecl
{
	explicit AstDecl(EDeclKind kind_in);

	EDeclKind kind;

	// PA6-terminal token span [begin_token, end_token) of the
	// declaration as parsed (including any trailing semicolon when the
	// declaration owns one). PA11 derives the deterministic mock names
	// of anonymous types from this span; the PA10 dump ignores it.
	size_t begin_token;
	size_t end_token;

	// translation-unit / namespace / linkage bodies
	std::vector<AstDeclPtr> body_decls;

	// simple-declaration and function-definition
	AstSpecifierSeq specifiers;
	std::vector<AstInitDeclarator> declarators;  // DK_SIMPLE
	AstDeclaratorPtr declarator;   // DK_FUNCTION, special members
	AstStmtPtr body;               // function/special-member compound

	// namespace-definition / namespace-alias / alias-declaration
	std::string name;
	bool inline_namespace;
	bool unnamed;
	AstName target;        // using-directive/declaration, alias target
	AstTypeIdPtr type;     // DK_ALIAS aliased type, DK_ENUM base
	std::string linkage;   // DK_LINKAGE language ("C", "C++")

	// class / enum
	bool has_name;
	AstName class_name;    // DK_CLASS / DK_CLASS_FORWARD
	ETokenType class_key;
	std::string class_key_spelling;
	std::vector<AstBaseSpecifier> bases;
	std::vector<AstDeclPtr> members;
	bool has_enum_key;
	ETokenType enum_key;
	std::string enum_key_spelling;
	bool has_enum_base;
	// True when the enum-specifier had a { } body (a definition, even if
	// empty), false for an opaque-enum-declaration.
	bool enum_body;
	std::vector<AstEnumerator> enumerators;

	// static_assert
	AstExprPtr assert_expr;
	bool has_message;
	std::string message;   // source spelling with quotes

	// access label
	ETokenType access;
	std::string access_spelling;

	// bit-fields: pairs of optional declarator and width
	struct BitField
	{
		AstDeclaratorPtr declarator;  // null when absent
		AstExprPtr width;
	};
	std::vector<BitField> bit_fields;

	// special members (the name annotation comes from the declarator-id)
	std::vector<AstMemberSpecifier> member_specifiers;
	AstInitializerPtr special_init;  // = default / = delete
	bool has_ctor_initializer;
	std::vector<AstMemInitializer> mem_initializers;

	// template-declaration / explicit instantiation
	bool has_parameter_list;
	std::vector<AstTemplateParameter> template_params;
	AstDeclPtr inner;  // templated declaration / instantiated declaration
};
