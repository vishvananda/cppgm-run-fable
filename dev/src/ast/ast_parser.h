#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "parse/parse_token.h"

// Recursive-descent tree-building parser for the pa10.gram source
// grammar over the PA6 terminal sequence, using the PA6 parse-function
// discipline: every Parse* member either succeeds with the lookahead
// one past its parse, or fails with the lookahead, bracket stack,
// scope stack, and name registrations restored to its entry state.
//
// 14.2.3 close-angle handling follows the PA6 parser: consuming
// (/[/{ pushes the bracket stack and the matching closer pops it;
// template argument/parameter lists push an angle context which
// close-angle-bracket (OP_GT, ST_RSHIFT_1, ST_RSHIFT_2) pops; the
// relational OP_GT and the split ST_RSHIFT pair refuse to act as
// operators while the innermost open bracket is an angle.
//
// On top of the grammar the parser keeps a syntactic name table: a
// scope stack of identifier -> {type, template, value} category
// entries fed by declarations as they parse, with persistent child
// tables for namespace and class scopes so qualified names like
// N::min can be classified. The table drives only the standard's
// syntactic disambiguations (type-name acceptance, the template-id
// versus less-than choice, declaration versus expression); it does no
// name lookup in the semantic sense.
class AstParser
{
public:
	explicit AstParser(const std::vector<ParseToken>& tokens);

	// Parses the whole token sequence as a translation-unit; throws
	// std::runtime_error when the sequence does not match.
	AstDeclPtr ParseTranslationUnit();

private:
	// --- name table -----------------------------------------------
	enum ENameFlags
	{
		NF_TYPE = 1 << 0,
		NF_VALUE = 1 << 1,
		NF_TEMPLATE = 1 << 2
	};

	struct NameTable
	{
		std::map<std::string, unsigned> entries;
		std::map<std::string, NameTable*> children;
	};

	struct ScopeRef
	{
		NameTable* table;
		bool param_scope;  // template-parameter scope
	};

	struct UndoEntry
	{
		NameTable* table;
		std::string name;
		bool existed;
		unsigned old_flags;
	};

	struct State
	{
		size_t pos;
		size_t bracket_depth;
		size_t scope_depth;
		size_t undo_depth;
	};

	// Result of classifying a name against the table: kResolvedMask is
	// set when the name resolved to an entry whose flags follow.
	static const int kUnresolved = -1;

	NameTable* NewTable();
	void PushScope(NameTable* table, bool param_scope);
	bool InTemplateScope() const;
	void PushTransientScope();
	void PopScope();
	void Register(const std::string& name, unsigned flags);
	void RegisterInDeclScope(const std::string& name, unsigned flags);
	NameTable* DeclScopeTable();
	NameTable* GetOrCreateChild(NameTable* parent, const std::string& name);
	// NF_TEMPLATE when a declaration's name is the direct subject of an
	// enclosing template header (so `B<T>` works inside B's own body).
	unsigned TemplatedFlag() const;
	int LookupUnqualified(const std::string& name) const;
	// Resolves a name's qualifier parts to their namespace/class table,
	// or null when any step cannot be classified syntactically.
	const NameTable* DescendPrefix(const AstName& prefix) const;
	// Flags of the entry a structured name resolves to, or kUnresolved
	// when any step (unknown root, decltype, missing member) cannot be
	// classified syntactically.
	int ResolveName(const AstName& name) const;
	int ResolveTerminalFlags(const AstName& prefix,
	                         const std::string& id) const;
	bool NameUsableAsType(const AstName& name) const;
	void RegisterDeclaratorIds(const AstDecl& decl);
	void RegisterParameters(const AstDeclarator& declarator);
	void RegisterTemplateName(const AstDecl& inner);

	// Outcome of a template-argument-clause attempt at one `<` token
	// position; end_pos is one past the close angle on success.
	struct ClauseMemo
	{
		bool success;
		size_t end_pos;
	};

	void InvalidateClauseMemo();
	void RecordClauseOutcome(size_t clause_pos, bool success);

	// --- token-stream infrastructure (ast_parser_core.cpp) ---------
	State Save() const;
	void Restore(const State& state);
	bool TokenAt(size_t index, ETokenType type) const;
	const ParseToken& Peek(size_t ahead = 0) const;
	void Advance();
	bool AtSimple(ETokenType type, size_t ahead = 0) const;
	bool AtIdentifier(size_t ahead = 0) const;
	bool AtIdentifierSpelled(const char* spelling) const;
	bool AtLiteral() const;
	bool AtEof() const;
	bool MatchSimple(ETokenType type);
	bool InAngleBrackets() const;
	bool MatchOpenAngle();
	bool AtCloseAngle() const;
	bool MatchCloseAngle();
	// __attribute__((...)) and alignas(...) adornments. abi_tag string
	// arguments record into `abi_tags` when the caller owns a
	// consuming declaration position; recorded nowhere else, so a
	// tag never leaks across declarations or tentative re-parses.
	void SkipDeclAdornments(std::vector<std::string>* abi_tags = 0);
	// Class-head adornments: __attribute__ forms are discarded,
	// alignas(...) specifiers are captured into the class declaration.
	void ParseClassAdornments(AstDecl& decl);
	bool SkipSquareAttribute();  // [[ ... ]]
	bool SkipBalancedParens();
	bool SkipAttributeParens(std::vector<std::string>* abi_tags);
	// [[no_unique_address]] seen by the last SkipSquareAttribute.
	bool last_no_unique_address_ = false;

	// --- names (ast_parse_names.cpp) --------------------------------
	// Template-id attempt policy for `name <`, from the name table:
	// known templates always try, known non-templates never do, and
	// unknown names try unconditionally in type contexts but commit in
	// expression contexts only when a `(` follows (the fixtures pin
	// this for member-access and comparison chains).
	bool ParseTemplateArgumentClause(AstNamePart& part);
	bool TemplateIdAllowed(int flags) const;
	bool ParseTerminalIdentifier(AstName& name, bool type_context);
	bool ParseNestedNameParts(AstName& name);
	bool ParseIdExpressionName(AstName& name);
	bool ParseQualifiedTypeName(AstName& name);
	bool ParseOperatorIdPart(AstNamePart& part);
	bool ParseConversionTypeId(AstTypeIdPtr& type);
	bool ParseTemplateArgument(AstTemplateArgument& argument);
	bool AtTemplateArgumentFollow() const;

	// --- expressions (ast_parse_expr.cpp) ---------------------------
	AstExprPtr ParseExpression();
	AstExprPtr ParseAssignmentExpression();
	AstExprPtr ParseConditionalExpression();
	AstExprPtr ParseOffsetofExpression();
	AstExprPtr ParseBinaryExpression(int level);
	bool MatchBinaryOperator(int level, ETokenType& op, std::string& spelling);
	AstExprPtr ParseCastExpression();
	AstExprPtr ParseUnaryExpression();
	AstExprPtr ParseSizeofExpression();
	AstExprPtr ParseTraitExpression(ETokenType keyword);
	AstExprPtr ParseNewExpression();
	AstExprPtr ParseDeleteExpression();
	AstExprPtr ParsePostfixExpression();
	AstExprPtr ParsePostfixRoot();
	AstExprPtr ParseBuiltinTraitExpression();  // PA34 __is_* ( types )
	AstExprPtr ParseFoldExpressionTail();      // PA34 ( pack op ... )
	bool MatchFoldOperator(ETokenType& op, std::string& spelling);
	bool FoldDotsAhead() const;
	// The trailing call/subscript/member/++/-- suffixes over `expr`.
	AstExprPtr ParsePostfixSuffixes(AstExprPtr expr);
	AstExprPtr ParsePrimaryExpression();
	AstExprPtr ParseLambdaExpression();
	bool ParseLambdaIntroducer(AstLambda& lambda);
	AstExprPtr ParseInitializerClause();
	bool ParseInitializerClauseList(std::vector<AstExprPtr>& list);
	AstExprPtr ParseBracedInitList();
	AstExprPtr ParseParenExprList(EExprKind kind);

	// --- statements (ast_parse_stmt.cpp) -----------------------------
	AstStmtPtr ParseStatement();
	AstStmtPtr ParseCompoundStatement();
	AstStmtPtr ParseBlockItem();
	AstStmtPtr ParseLabeledStatement();
	AstStmtPtr ParseSelectionStatement();
	AstStmtPtr ParseIterationStatement();
	AstStmtPtr ParseJumpStatement();
	AstStmtPtr ParseTryBlock();
	// PA29 function-try-block: `try ctor-initializer? compound
	// handler-seq` as a function body (KW_TRY at the current token).
	AstStmtPtr ParseFunctionTryBody(AstDecl& decl);
	bool ParseHandlerSeq(AstStmt& stmt);
	AstStmtPtr ParseExpressionStatement();
	AstConditionPtr ParseCondition();
	AstStmtPtr ParseForInitStatement();
	// PA24 6.5.4: the range form (after `for (`).
	AstStmtPtr ParseRangeForForm(bool& matched);

	// --- specifiers and declarators (ast_parse_declarator.cpp) -------
	enum ESeqKind
	{
		kDeclSpecifierSeq,  // storage/function specifiers allowed
		kTypeSpecifierSeq   // types and cv only
	};
	bool ParseSpecifierSeq(AstSpecifierSeq& seq, ESeqKind kind);
	bool ParseOneSpecifier(AstSpecifierSeq& seq, ESeqKind kind,
	                       int& type_state);
	bool ParseTypeId(AstTypeIdPtr& type);
	bool ParseDeclarator(AstDeclaratorPtr& declarator, bool named);
	bool ParsePtrOperators(AstDeclarator& declarator);
	bool ParseDirectDeclarator(AstDeclarator& declarator, bool named);
	bool ParseDeclaratorSuffixes(AstDeclarator& declarator);
	bool ParseParameterClause(AstParameterClausePtr& clause);
	bool ParseParameterDeclaration(AstParameter& parameter);
	bool AtParameterFollow() const;
	bool ParseFunctionQualifiers(AstDeclarator& declarator);
	bool ParseInitializer(AstInitializerPtr& init, bool allow_paren);
	bool ParseBraceOrEqualInitializer(AstInitializerPtr& init);

	// --- declarations (ast_parse_decl.cpp) ---------------------------
	AstDeclPtr ParseDeclaration();      // ParseDeclarationForms + span
	AstDeclPtr ParseDeclarationForms();
	AstDeclPtr ParseNamespaceDefinition();
	AstDeclPtr ParseNamespaceAliasDefinition();
	AstDeclPtr ParseUsingDeclarationOrDirective();
	AstDeclPtr ParseAliasDeclaration();
	bool AtStructuredBindingIntro() const;
	bool ParseStructuredBindingIntro(AstDecl& decl);
	AstDeclPtr ParseStructuredBinding();
	AstDeclPtr ParseLinkageSpecification();
	AstDeclPtr ParseExplicitInstantiation();
	AstDeclPtr ParseStaticAssertDeclaration();
	AstDeclPtr ParseSimpleDeclaration();
	AstDeclPtr ParseFunctionDefinition();

	// --- classes, enums, templates (ast_parse_class.cpp) -------------
	AstDeclPtr ParseClassDeclaration();    // forward or definition + ;
	AstDeclPtr ParseClassSpecifier();      // with body, as a specifier
	AstDeclPtr ParseElaboratedClass();     // class-key name, no body
	bool ParseClassBody(AstDecl& decl);
	bool ParseBaseClause(AstDecl& decl);
	AstDeclPtr ParseMemberDeclaration();  // forms + token span
	AstDeclPtr ParseMemberDeclarationForms();
	// PA21: registers later-declared member templates' names before
	// the members parse (template classification for `<`).
	void PreScanMemberTemplates(const std::string& class_name);
	// PA35: a base class's registered names are visible unqualified in
	// the derived body (`has_x<T>` inherited from a base must classify
	// as a template for the `<` disambiguation). Copies entry flags
	// only; unresolvable (e.g. dependent) bases are skipped.
	void ImportBaseEntries(const AstDecl& decl);
	AstDeclPtr ParseBitFieldDeclaration();
	AstDeclPtr ParseSpecialMember(bool require_definition,
	                              bool qualified_default_only = false);
	bool ParseSpecialMemberName(AstName& name);
	bool ParseCtorInitializer(AstDecl& decl);
	AstDeclPtr ParseEnumDeclaration();     // enum-specifier + ;
	AstDeclPtr ParseEnumSpecifier();
	AstDeclPtr ParseTemplateDeclaration();
	bool ParseTemplateParameterList(std::vector<AstTemplateParameter>& params);
	AstDeclPtr ParseDeductionGuide();
	bool ParseTemplateParameter(AstTemplateParameter& parameter);
	bool ParseTypeParameter(AstTemplateParameter& parameter);
	bool ParseNonTypeParameter(AstTemplateParameter& parameter);

	// PA34: GNU alias keywords (__signed, __const, __inline__, __thread,
	// __decltype, ...) normalize to their standard keyword tokens in an
	// owned copy; without aliases the parser reads the caller's vector.
	static const std::vector<ParseToken>& NormalizeGnuAliasTokens(
		const std::vector<ParseToken>& tokens,
		std::vector<ParseToken>& storage);
	std::vector<ParseToken> gnu_normalized_;
	const std::vector<ParseToken>& tokens_;
	size_t pos_;
	std::vector<char> brackets_;
	std::deque<NameTable> table_pool_;
	std::vector<ScopeRef> scopes_;
	std::vector<UndoEntry> undo_log_;
	// Clause attempt outcomes by `<` position, valid for the current
	// name-table and scope state (any change invalidates). A clause
	// parse depends only on its tokens and that state, so the memo lets
	// uncommitted attempts skip re-parsing: without it, nested
	// template-argument ambiguities re-parse the same span once per
	// enclosing alternative, which compounds exponentially.
	std::map<size_t, ClauseMemo> clause_memo_;
	// Scope depth of the innermost template header whose declaration is
	// being parsed; a name registered at exactly this depth is the
	// templated entity itself. Zero when not inside a template header.
	size_t template_decl_scope_depth_;
};
