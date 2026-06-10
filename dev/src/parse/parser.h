#pragma once

#include <cstddef>
#include <vector>

using std::size_t;
using std::vector;

#include "parse_token.h"
#include "parse_tree.h"

// Recursive-descent recognizer for the pa6.gram translation-unit
// grammar over the PA6 terminal sequence, using the handout's parse
// function discipline: every Parse* member either returns a node with
// the lookahead one past its parse, or null with the lookahead (and
// the bracket-context stack) restored to its entry state.
//
// Ambiguities are resolved per the standard's disambiguation rules:
// declaration before expression for statements (6.8),
// parameters-and-qualifiers before a parenthesized initializer (8.2),
// the abstract (type) reading before the named reading inside a
// parameter-declaration (8.2/7), and type-id before expression for
// template arguments, sizeof, typeid, and alignas (8.2/3). Alternatives
// whose choice the grammar cannot validate locally are checked against
// the relevant FOLLOW tokens before committing (template-argument,
// parameter-declaration, member-declarator).
//
// 14.2.3 close-angle-bracket handling: brackets_ tracks the innermost
// open bracket. Consuming (/[/{ pushes and the matching closer pops
// (the grammar opens and closes hard brackets within one alternative,
// so no parse function pops below its entry depth and backtracking
// restores by truncation). Template-argument/parameter lists and
// cast<...> push an angle context; close-angle-bracket (OP_GT,
// ST_RSHIFT_1, or ST_RSHIFT_2) pops it. The relational OP_GT and the
// shift ST_RSHIFT_1 ST_RSHIFT_2 operators refuse to match while the
// innermost bracket is an angle, reserving those tokens for
// close-angle-bracket at the same nesting level.
class Parser
{
public:
	explicit Parser(const vector<ParseToken>& tokens);

	// Parses the whole token sequence as translation-unit; null means
	// the file does not match (BAD).
	ParseNodePtr ParseTranslationUnit();

private:
	struct State
	{
		size_t pos;
		size_t bracket_depth;
	};

	typedef ParseNodePtr (Parser::*ParseFn)();

	// The rules with failure memoization: the chokepoints every
	// ordered-choice retry re-descends through (the type/expression/id
	// readings of template-argument, sizeof, typeid, and alignas; the
	// declaration/expression readings of statements and conditions; the
	// abstract/named declarator readings of parameters). One slot per
	// (rule, position, innermost-bracket-is-angle); see MemoParse.
	enum EMemoRule
	{
		kMemoSimpleTemplateId,
		kMemoIdExpression,
		kMemoNestedNameSpecifier,
		kMemoTypeId,
		kMemoDeclarator,
		kMemoAbstractDeclarator,
		kMemoParametersAndQualifiers,
		kMemoDeclSpecifierSeq,
		kMemoTypeSpecifierSeq,
		kMemoTrailingTypeSpecifierSeq,
		kMemoConditionalExpression,
		kMemoAssignmentExpression,
		kMemoExpression,
		kMemoCastExpression,
		kMemoUnaryExpression,
		kMemoBlockDeclaration,
		kNumMemoRules
	};

	// --- token-stream infrastructure (parser_core.cpp) ---
	ParseNodePtr MemoParse(EMemoRule rule_index, ParseFn rule);
	State Save() const;
	void Restore(const State& state);
	const ParseToken& Peek(size_t ahead = 0) const;
	void Advance();
	bool AtSimple(ETokenType type, size_t ahead = 0) const;
	bool AtIdentifier(size_t ahead = 0) const;
	bool AtLiteral() const;
	bool AtEof() const;
	bool MatchSimple(ETokenType type);
	ParseNodePtr MatchSimpleLeaf(ETokenType type);
	ParseNodePtr MatchIdentifierLeaf();
	ParseNodePtr MatchLiteralLeaf();
	bool InAngleBrackets() const;
	bool MatchOpenAngle();
	ParseNodePtr ParseCloseAngleBracket();
	ParseNodePtr ParseCommaList(const char* name, ParseFn element);
	ParseNodePtr ParseDotsItem(const char* name, ParseFn element);

	// --- names and templates (parse_names.cpp) ---
	ParseNodePtr ParseTypeName();
	ParseNodePtr ParseClassName();
	ParseNodePtr ParseNamespaceName();
	ParseNodePtr ParseSimpleTemplateId();
	ParseNodePtr ParseSimpleTemplateIdRule();
	ParseNodePtr ParseIdExpression();
	ParseNodePtr ParseIdExpressionRule();
	ParseNodePtr ParseUnqualifiedId();
	ParseNodePtr ParseQualifiedId();
	ParseNodePtr ParseNestedNameSpecifier();
	ParseNodePtr ParseNestedNameSpecifierRule();
	ParseNodePtr ParseNestedNameSpecifierRoot();
	ParseNodePtr ParseNestedNameSpecifierSuffix();
	ParseNodePtr ParseDecltypeSpecifier();
	ParseNodePtr ParseTypenameSpecifier();
	ParseNodePtr ParseOperatorIdForm();
	ParseNodePtr ParseOperatorFunctionId();
	ParseNodePtr ParseConversionFunctionId();
	ParseNodePtr ParseConversionTypeId();
	ParseNodePtr ParseLiteralOperatorId();
	ParseNodePtr ParseTemplateArgumentList();
	ParseNodePtr ParseTemplateArgumentDots();
	ParseNodePtr ParseTemplateArgument();
	bool AtTemplateArgumentFollow() const;

	// --- expressions (parse_expr.cpp) ---
	ParseNodePtr ParsePrimaryExpression();
	ParseNodePtr ParseLambdaExpression();
	ParseNodePtr ParseLambdaIntroducer();
	ParseNodePtr ParseLambdaCapture();
	ParseNodePtr ParseCaptureList();
	ParseNodePtr ParseCaptureDots();
	ParseNodePtr ParseCapture();
	ParseNodePtr ParseLambdaDeclarator();
	ParseNodePtr ParsePostfixExpression();
	ParseNodePtr ParsePostfixRoot();
	ParseNodePtr ParsePostfixSuffix();
	ParseNodePtr ParseExpressionList();
	ParseNodePtr ParsePseudoDestructorName();
	ParseNodePtr ParseUnaryExpression();
	ParseNodePtr ParseUnaryExpressionRule();
	ParseNodePtr ParseSizeofExpression();
	ParseNodePtr ParseNewExpression();
	ParseNodePtr ParseNewPlacement();
	ParseNodePtr ParseNewTypeId();
	ParseNodePtr ParseNewDeclarator();
	ParseNodePtr ParseNoptrNewDeclarator();
	ParseNodePtr ParseNewInitializer();
	ParseNodePtr ParseDeleteExpression();
	ParseNodePtr ParseCastExpression();
	ParseNodePtr ParseCastExpressionRule();
	ParseNodePtr ParseBinaryExpression(int level);
	ParseNodePtr ParseBinaryOperator(int level);
	ParseNodePtr ParseConditionalSuffix(ParseNodePtr condition);
	ParseNodePtr ParseConditionalExpression();
	ParseNodePtr ParseConditionalExpressionRule();
	ParseNodePtr ParseAssignmentExpression();
	ParseNodePtr ParseAssignmentExpressionRule();
	ParseNodePtr ParseExpression();
	ParseNodePtr ParseExpressionRule();
	ParseNodePtr ParseConstantExpression();
	ParseNodePtr ParseThrowExpression();
	ParseNodePtr ParseInitializerClause();
	ParseNodePtr ParseInitializerClauseDots();
	ParseNodePtr ParseInitializerList();
	ParseNodePtr ParseBracedInitList();

	// --- statements (parse_stmt.cpp) ---
	ParseNodePtr ParseStatement();
	ParseNodePtr ParseExpressionStatement();
	ParseNodePtr ParseCompoundStatement();
	ParseNodePtr ParseSelectionStatement();
	ParseNodePtr ParseCondition();
	ParseNodePtr ParseConditionDeclaration();
	ParseNodePtr ParseIterationStatement();
	ParseNodePtr ParseForSuffix();
	ParseNodePtr ParseForInitStatement();
	ParseNodePtr ParseForRangeDeclaration();
	ParseNodePtr ParseForRangeInitializer();
	ParseNodePtr ParseJumpStatement();
	ParseNodePtr ParseTryBlock();
	ParseNodePtr ParseHandler();
	ParseNodePtr ParseExceptionDeclaration();

	// --- declarations (parse_decl.cpp) ---
	ParseNodePtr ParseDeclaration();
	ParseNodePtr ParseBlockDeclaration();
	ParseNodePtr ParseBlockDeclarationRule();
	ParseNodePtr ParseSimpleDeclaration();
	ParseNodePtr ParseStaticAssertDeclaration();
	ParseNodePtr ParseAttributeDeclaration();
	ParseNodePtr ParseAliasDeclaration();
	ParseNodePtr ParseUsingDeclaration();
	ParseNodePtr ParseUsingDirective();
	ParseNodePtr ParseAsmDefinition();
	ParseNodePtr ParseNamespaceDefinition();
	ParseNodePtr ParseNamespaceAliasDefinition();
	ParseNodePtr ParseQualifiedNamespaceSpecifier();
	ParseNodePtr ParseLinkageSpecification();
	ParseNodePtr ParseTemplateDeclaration();
	ParseNodePtr ParseTemplateParameter();
	ParseNodePtr ParseTypeParameter();
	ParseNodePtr ParseExplicitInstantiation();
	ParseNodePtr ParseExplicitSpecialization();
	bool ParseAttributeSpecifiers(ParseNode* parent);
	ParseNodePtr ParseAttributeSpecifier();
	ParseNodePtr ParseAlignmentSpecifier();
	ParseNodePtr ParseAttributeList();
	ParseNodePtr ParseAttribute();
	ParseNodePtr ParseAttributeArgumentClause();
	ParseNodePtr ParseBalancedToken();

	// --- specifier sequences (parse_decl.cpp) ---
	enum ESpecifierSeqKind
	{
		kDeclSpecifierSeq,
		kTypeSpecifierSeq,
		kTrailingTypeSpecifierSeq
	};
	ParseNodePtr ParseDeclSpecifierSeq();
	ParseNodePtr ParseDeclSpecifierSeqRule();
	ParseNodePtr ParseTypeSpecifierSeq();
	ParseNodePtr ParseTypeSpecifierSeqRule();
	ParseNodePtr ParseTrailingTypeSpecifierSeq();
	ParseNodePtr ParseTrailingTypeSpecifierSeqRule();
	ParseNodePtr ParseSpecifierSeq(ESpecifierSeqKind kind, const char* name);
	ParseNodePtr ParseOneSpecifier(ESpecifierSeqKind kind, bool* seen_type);
	ParseNodePtr ParseTaggedTypeSpecifier(ESpecifierSeqKind kind,
	                                      bool* seen_type);
	ParseNodePtr ParseSimpleTypeSpecifier();
	ParseNodePtr ParseTypeNameSpecifier();
	ParseNodePtr ParseElaboratedTypeSpecifier();
	ParseNodePtr ParseEnumSpecifier();
	ParseNodePtr ParseEnumHead();
	ParseNodePtr ParseEnumKey();
	ParseNodePtr ParseEnumBase();
	ParseNodePtr ParseEnumeratorDefinition();
	ParseNodePtr ParseOpaqueEnumDeclaration();

	// --- declarators (parse_declarator.cpp) ---
	ParseNodePtr ParseInitDeclarator();
	ParseNodePtr ParseDeclarator();
	ParseNodePtr ParseDeclaratorRule();
	ParseNodePtr ParsePtrDeclarator();
	ParseNodePtr ParseNoptrDeclarator();
	ParseNodePtr ParseNoptrDeclaratorSuffix();
	ParseNodePtr ParseParametersAndQualifiers();
	ParseNodePtr ParseParametersAndQualifiersRule();
	ParseNodePtr ParseTrailingReturnType();
	ParseNodePtr ParsePtrOperator();
	ParseNodePtr ParseCvQualifier();
	ParseNodePtr ParseRefQualifier();
	ParseNodePtr ParseDeclaratorId();
	ParseNodePtr ParseTypeId();
	ParseNodePtr ParseTypeIdRule();
	ParseNodePtr ParseAbstractDeclarator();
	ParseNodePtr ParseAbstractDeclaratorRule();
	ParseNodePtr ParsePtrAbstractDeclarator();
	ParseNodePtr ParseNoptrAbstractDeclarator();
	ParseNodePtr ParseAbstractPackDeclarator();
	ParseNodePtr ParseParameterDeclarationClause();
	ParseNodePtr ParseParameterDeclaration();
	bool AtParameterDeclarationFollow() const;
	ParseNodePtr ParseFunctionDefinition();
	ParseNodePtr ParseFunctionBody();
	ParseNodePtr ParseFunctionTryBlock();
	ParseNodePtr ParseInitializer();
	ParseNodePtr ParseBraceOrEqualInitializer();
	ParseNodePtr ParseExceptionSpecification();
	ParseNodePtr ParseTypeIdDots();

	// --- classes and members (parse_class.cpp) ---
	ParseNodePtr ParseClassSpecifier();
	ParseNodePtr ParseClassHead();
	ParseNodePtr ParseClassHeadName();
	ParseNodePtr ParseClassKey();
	ParseNodePtr ParseMemberSpecification();
	ParseNodePtr ParseMemberDeclaration();
	ParseNodePtr ParseMemberDeclarator();
	ParseNodePtr ParseVirtSpecifier();
	ParseNodePtr ParseBaseClause();
	ParseNodePtr ParseBaseSpecifierDots();
	ParseNodePtr ParseBaseSpecifier();
	ParseNodePtr ParseClassOrDecltype();
	ParseNodePtr ParseAccessSpecifier();
	ParseNodePtr ParseCtorInitializer();
	ParseNodePtr ParseMemInitializerDots();
	ParseNodePtr ParseMemInitializer();

	const vector<ParseToken>& tokens_;
	size_t pos_;
	vector<char> brackets_;
	vector<bool> rule_failed_;  // MemoParse bitmap, one bit per slot
};
