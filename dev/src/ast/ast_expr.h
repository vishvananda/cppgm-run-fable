#pragma once

#include "ast/ast_names.h"

// The PA10 syntax tree, part 2: expressions, lambdas, and statements.

enum EExprKind
{
	EK_LITERAL,          // TT_LITERAL leaf
	EK_KEYWORD_LITERAL,  // true false nullptr this
	EK_ID,               // id-expression
	EK_PAREN,            // ( expression )
	EK_BINARY,           // all binary levels including comma
	EK_ASSIGNMENT,       // assignment-operator form
	EK_CONDITIONAL,      // ?:
	EK_UNARY,            // prefix unary operators
	EK_POSTFIX_INCDEC,   // postfix ++ --
	EK_CALL,             // postfix ( argument-list? )
	EK_FUNCTIONAL_CAST,  // simple-type-keyword ( arguments )
	EK_SUBSCRIPT,        // postfix [ expression ]
	EK_MEMBER,           // postfix . or -> id
	EK_CSTYLE_CAST,      // ( type-id ) cast-expression
	EK_KEYWORD_CAST,     // static_cast<...>(...) etc
	EK_SIZEOF_EXPR,      // sizeof unary-expression
	EK_SIZEOF_TYPE,      // sizeof ( type-id )
	EK_SIZEOF_PACK,      // sizeof ... ( identifier )
	EK_TYPE_TRAIT,       // typeid / alignof / noexcept ( ... )
	EK_NEW,              // new-expression
	EK_DELETE,           // delete-expression
	EK_LAMBDA,           // lambda-expression
	EK_PACK_EXPANSION,   // initializer-clause ...
	EK_BRACED,           // braced-init-list
	EK_THROW,            // throw-expression (15.1)
	EK_STATEMENT_EXPR,   // GNU ( compound-statement ) expression
	EK_VA_ARG,           // __builtin_va_arg ( expr , type-id )
	EK_BUILTIN_TRAIT     // PA34 __is_*/__has_* ( type-id-list )
};

struct AstExpr
{
	explicit AstExpr(EExprKind kind_in);

	EExprKind kind;
	ETokenType op;             // operator / cast keyword / trait keyword
	std::string op_spelling;
	std::vector<AstExprPtr> operands;  // operands in source order
	std::vector<AstExprPtr> arguments; // call/cast/placement arguments
	AstName name;              // EK_ID; EK_MEMBER member name
	std::string literal;       // EK_LITERAL / EK_KEYWORD_LITERAL spelling
	// EK_LITERAL phase-7 facts copied from the terminal (PTK_LITERAL /
	// PTK_LITERAL_ARRAY / user-defined kinds, scalar or element type,
	// element count, ABI value bytes). The PA10 dump ignores them; the
	// PA12 semantic pass reads them instead of re-lexing the spelling.
	EPostTokenKind literal_kind;
	EFundamentalType literal_type;
	size_t literal_elements;
	std::string literal_data;
	std::string literal_suffix;  // user-defined literal kinds
	// EK_FUNCTIONAL_CAST: the simple-type keyword sequence (one entry
	// for the classic single-keyword form, several for the PA12
	// multi-keyword form such as `unsigned long(e)`).
	std::vector<ETokenType> cast_keywords;
	AstTypeIdPtr type;         // casts, sizeof(type), traits, new
	// EK_BUILTIN_TRAIT operands: type-id arguments in source order,
	// each optionally pack-expanded (`Args...`).
	std::vector<AstTemplateArgument> trait_args;
	bool is_type_operand;      // EK_TYPE_TRAIT operand is a type-id
	bool sizeof_paren;         // sizeof ( expression ) form
	bool global_scope;         // EK_NEW / EK_DELETE leading ::
	bool array_delete;         // delete [ ]
	bool has_placement;        // EK_NEW placement arguments present
	bool paren_type;           // EK_NEW ( type-id ) form
	AstInitializerPtr new_init;  // EK_NEW initializer
	AstLambdaPtr lambda;       // EK_LAMBDA
	AstStmtPtr stmt_body;      // EK_STATEMENT_EXPR compound statement
};

// lambda-introducer capture: identifier, & identifier, or this,
// optionally a pack expansion.
enum ELambdaCaptureKind
{
	LC_THIS,  // this
	LC_COPY,  // identifier
	LC_REF    // & identifier
};

struct AstLambdaCapture
{
	AstLambdaCapture();

	ELambdaCaptureKind kind;
	std::string identifier;
	bool pack;
};

struct AstLambda
{
	AstLambda();

	// PA25: the PA6-terminal token span [declarator-begin, body-begin)
	// of the lambda-declarator; deterministic closure-class names
	// derive from it.
	size_t declarator_begin_token = 0;
	size_t body_begin_token = 0;

	bool has_capture_default;
	ETokenType capture_default;  // OP_AMP / OP_ASS
	std::vector<AstLambdaCapture> captures;
	bool has_declarator;
	AstParameterClausePtr parameters;
	bool mutable_specifier;
	std::string mutable_spelling;
	bool has_noexcept;
	AstExprPtr noexcept_expr;  // may be null for bare noexcept
	bool has_trailing;
	AstTypeIdPtr trailing_type;
	AstStmtPtr body;
};

// --- statements -------------------------------------------------------

enum EStmtKind
{
	SK_COMPOUND,
	SK_DECLARATION,
	SK_EXPRESSION,
	SK_IF,
	SK_SWITCH,
	SK_WHILE,
	SK_DO,
	SK_FOR,
	SK_BREAK,
	SK_CONTINUE,
	SK_GOTO,
	SK_RETURN,
	SK_THROW,
	SK_TRY,
	SK_LABELED,
	SK_CASE,
	SK_DEFAULT
};

// if/switch/while/for/do condition: an expression or the
// declaration form with a required initializer.
struct AstCondition
{
	AstCondition();

	bool is_declaration;
	AstExprPtr expr;
	AstSpecifierSeq specifiers;
	AstDeclaratorPtr declarator;
	AstInitializerPtr init;
};

typedef std::unique_ptr<AstCondition> AstConditionPtr;

struct AstHandler
{
	AstHandler();

	bool ellipsis;
	AstSpecifierSeq specifiers;
	AstDeclaratorPtr declarator;  // may be null or abstract
	AstStmtPtr body;
};

struct AstStmt
{
	explicit AstStmt(EStmtKind kind_in);

	EStmtKind kind;
	std::vector<AstStmtPtr> items;  // SK_COMPOUND block items
	AstDeclPtr decl;                // SK_DECLARATION
	AstExprPtr expr;                // expression/return/throw/case value
	std::string label;              // SK_GOTO, SK_LABELED
	AstConditionPtr condition;      // if/switch/while/do/for
	AstStmtPtr then_branch;         // SK_IF
	AstStmtPtr else_branch;         // SK_IF
	AstStmtPtr body;                // loop/switch/labeled/case bodies, try compound
	AstStmtPtr for_init;            // SK_FOR (declaration or expression stmt)
	AstExprPtr iteration;           // SK_FOR third clause
	// SK_FOR range form (6.5.4): the for-range-declaration (one
	// uninitialized declarator) and the for-range-initializer.
	AstDeclPtr for_range_decl;
	AstExprPtr for_range_init;
	std::vector<AstHandler> handlers;  // SK_TRY
	bool function_try = false;         // SK_TRY as a function body
};
