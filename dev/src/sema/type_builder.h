#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

#include "ast/ast.h"
#include "sema/type.h"

// PA11 declarator-derived type construction: decl-specifier-seq
// processing, the clause 8 declarator composition over the PA10 item
// lists, and type-id resolution. The declaration side effects of
// specifiers (defining nested classes/enums, looking names up in the
// scope model, evaluating array bounds) belong to the binder, which
// the builder reaches through ITypeBuilderHost - so this module owns
// only the structural rules.

struct ITypeBuilderHost
{
	// A class-specifier, enum-specifier, or elaborated forward
	// declaration used inside a specifier sequence: bind it and return
	// the type it names.
	virtual TypePtr BindNestedTypeSpecifier(const AstDecl& decl) = 0;
	// Lookup of a possibly qualified type name (throws when the name
	// does not name a type).
	virtual TypePtr ResolveTypeName(const AstName& name) = 0;
	// The supported decltype forms (7.1.6.2p4 subset).
	virtual TypePtr ResolveDecltype(const AstExpr& expr) = 0;
	// 8.3.4: the value of an array-bound constant expression.
	virtual unsigned long long EvaluateArrayBound(const AstExpr& expr) = 0;
	virtual ~ITypeBuilderHost() {}
};

// Storage and function-specifier facts of one decl-specifier-seq, with
// the base type (sequence cv applied).
struct DeclSpecifierInfo
{
	DeclSpecifierInfo()
		: is_auto(false), is_typedef(false), is_static(false),
		  is_extern(false), is_thread_local(false), is_inline(false),
		  is_virtual(false), is_constexpr(false), is_friend(false),
		  is_mutable(false)
	{}

	TypePtr type;
	bool is_auto;  // `auto` placeholder (trailing-return declarators)
	bool is_typedef;
	bool is_static;
	bool is_extern;
	bool is_thread_local;
	bool is_inline;
	bool is_virtual;
	bool is_constexpr;
	// PA15 member specifiers.
	bool is_friend;
	bool is_mutable;
};

// One declared parameter (PA11 keeps the declared type; no 8.3.5p5
// adjustment - the fixtures pin `function of (array of 3 int)`).
struct ParameterInfo
{
	ParameterInfo() : default_arg(0) {}

	string name;  // empty when unnamed
	TypePtr type;
	// PA14: `= expression` default argument (null when absent); points
	// into the translation unit's AST, used while it is alive.
	const AstExpr* default_arg;
};

// The composition result of one declarator over a base type.
struct DeclaratorInfo
{
	DeclaratorInfo() : id(0), declares_function(false),
	                   noexcept_simple(false) {}

	TypePtr type;
	// 8.3.5p2: a pending trailing-return-type replacing an `auto`
	// placeholder return (consumed by the parameter-clause suffix).
	TypePtr trailing_return;
	const AstName* id;  // declarator-id, null for abstract declarators
	// Set when the declarator's outermost type came directly from a
	// parameter clause (a function declarator, not a pointer/array of
	// functions); `parameters` then holds that clause's parameters for
	// a definition to bind.
	bool declares_function;
	vector<ParameterInfo> parameters;
	// PA14: bare `noexcept` or empty `throw()` on the declarator (the
	// cheap non-unwinding markings; expressions are not evaluated).
	bool noexcept_simple;
};

class TypeBuilder
{
public:
	explicit TypeBuilder(ITypeBuilderHost& host);

	// PA12 mode: function types apply the 8.3.5p5 parameter adjustment
	// (array/function decay, top-level cv deletion); the declared
	// parameter objects keep their cv. PA11 keeps declared types (its
	// fixtures pin `function of (array of 3 int)`).
	void SetParameterAdjustment(bool adjust);

	// decl-specifier-seq (allow_storage) or type-specifier-seq.
	DeclSpecifierInfo ProcessSpecifiers(const AstSpecifierSeq& seq,
	                                    bool allow_storage);

	// Clause 8 composition; `declarator` may be null (pure base type).
	DeclaratorInfo ComposeDeclarator(const AstDeclarator* declarator,
	                                 const TypePtr& base);

	// 8.4 type-id: type-specifier-seq + abstract declarator.
	TypePtr ResolveTypeId(const AstTypeId& type_id);

private:
	void ConsumeSpecifierKeyword(const AstSpecifier& spec,
	                             bool allow_storage,
	                             DeclSpecifierInfo& info,
	                             SimpleTypeSpecifiers& simple,
	                             bool& is_const, bool& is_volatile);
	void ComposeItems(const vector<AstDeclaratorItem>& items,
	                  bool collapsible, DeclaratorInfo& out);
	void ApplyDeclaratorSuffix(const AstDeclaratorItem& item,
	                           bool fn_const, bool fn_volatile,
	                           DeclaratorInfo& out);
	void BuildParameters(const AstParameterClause& clause,
	                     vector<ParameterInfo>& parameters,
	                     vector<TypePtr>& types);

	ITypeBuilderHost& host_;
	bool adjust_parameters_;
};
