#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using std::size_t;
using std::string;
using std::unique_ptr;
using std::vector;

#include "post_token.h"
#include "sema/entity.h"
#include "sema/name_lookup.h"

// PA7 semantic parser: one forward recursive-descent pass over the
// pa7.gram translation-unit grammar that performs the semantic actions
// inline (7.3 namespace forms, declaration matching, type composition
// per clause 8), populating a SemaModel. It consumes the phase-7
// PostToken sequence directly because array bounds need the literal's
// type and value bytes.
//
// The grammar's choice points are all resolved predictively from typed
// lookup state, never by backtracking over semantic actions:
//  - declaration alternatives by one or two tokens of lookahead;
//  - an identifier in specifier position is a type-name exactly when
//    no type-specifier has been seen yet (every pa7 declaration has
//    exactly one type);
//  - '(' inside an abstract-capable declarator starts a parameter
//    clause or a parenthesized declarator per the 8.2p7 rule, deciding
//    by the following token and, for identifiers, by whether the
//    qualified name resolves to a typedef (a positional scan with no
//    side effects).
//
// Inputs outside the PA7 defined-behaviour contract (not matching
// pa7.gram, ill-formed, overloads, lookup after a qualified
// declarator-id) throw runtime_error; the tool exits EXIT_FAILURE.
class DeclParser
{
public:
	DeclParser(const vector<PostToken>& tokens, SemaModel& model);

	// Parses the whole token sequence, building the model's namespaces
	// and entities.
	void ParseTranslationUnit();

private:
	// An optionally qualified name: the nested-name-specifier components
	// (each a namespace-name in pa7) and the final unqualified-id.
	struct QualifiedName
	{
		QualifiedName() : root_global(false) {}

		bool Qualified() const
		{
			return root_global || !path.empty();
		}

		bool root_global;      // leading ::
		vector<string> path;
		string name;
	};

	// One type-composition step of a declarator, in source order.
	struct DeclaratorChunk
	{
		enum EKind { CK_POINTER, CK_LVALUE_REF, CK_RVALUE_REF, CK_FUNCTION,
		             CK_ARRAY };

		DeclaratorChunk() : kind(CK_POINTER), is_const(false),
		                    is_volatile(false), variadic(false),
		                    bound_known(false), bound(0) {}

		EKind kind;
		bool is_const;               // CK_POINTER cv
		bool is_volatile;
		vector<TypePtr> parameters;  // CK_FUNCTION, already adjusted
		bool variadic;               // CK_FUNCTION
		bool bound_known;            // CK_ARRAY
		unsigned long long bound;    // CK_ARRAY when bound_known
	};

	// Parsed declarator syntax: ptr-operators, then either a
	// declarator-id or a parenthesized sub-declarator (or neither, when
	// abstract), then array/function suffixes.
	struct Declarator
	{
		Declarator() : has_name(false) {}

		vector<DeclaratorChunk> prefix;  // ptr-operators, source order
		unique_ptr<Declarator> inner;    // parenthesized sub-declarator
		bool has_name;
		QualifiedName name;              // when has_name
		vector<DeclaratorChunk> suffixes;
	};

	enum EDeclaratorMode
	{
		DM_NAMED,      // simple-declaration: declarator-id required
		DM_PARAMETER,  // parameter-declaration: named or abstract
		DM_ABSTRACT    // type-id: abstract only
	};

	struct DeclSpecifiers
	{
		DeclSpecifiers() : is_typedef(false) {}

		TypePtr type;
		bool is_typedef;
	};

	// Accumulated decl-specifier-seq facts before combination per the
	// 7.1.6.2 table.
	struct SpecifierState
	{
		SpecifierState() : signed_count(0), unsigned_count(0),
		                   short_count(0), long_count(0), has_base(false),
		                   base(KW_INT), is_const(false), is_volatile(false),
		                   is_typedef(false) {}

		int signed_count;
		int unsigned_count;
		int short_count;
		int long_count;
		bool has_base;
		ETokenType base;
		bool is_const;
		bool is_volatile;
		TypePtr named;  // type-name specifier, resolved
		bool is_typedef;
	};

	// --- token stream ---
	const PostToken& Peek(size_t ahead = 0) const;
	void Advance();
	bool AtSimple(ETokenType type, size_t ahead = 0) const;
	bool AtIdentifier(size_t ahead = 0) const;
	void ExpectSimple(ETokenType type);
	string ExpectIdentifier();
	std::runtime_error ParseError(const string& message) const;

	// --- declarations ---
	void ParseDeclarationSeq();
	void ParseDeclaration();
	void ParseNamespaceDefinition();
	void ParseNamespaceAlias();
	void ParseUsingDirective();
	void ParseUsingDeclaration();
	void ParseAliasDeclaration();
	void ParseSimpleDeclaration();
	void DeclareSimple(const DeclSpecifiers& specs, const Declarator& d);
	void BindTypedef(const string& name, const TypePtr& type);

	// --- names ---
	QualifiedName ParseIdExpression();
	Namespace* ParseNamespaceSpecifier();
	TypePtr ResolveTypeName(const QualifiedName& name) const;
	// Resolves an optionally qualified name (null when any step fails):
	// unqualified lookup for a lone identifier, otherwise the
	// nested-name-specifier chain then qualified lookup of `last`.
	const Binding* ResolveComponents(bool root_global,
	                                 const vector<string>& path,
	                                 const string& last,
	                                 ELookupFilter filter) const;

	// --- specifiers ---
	DeclSpecifiers ParseDeclSpecifierSeq(bool allow_decl_specifiers);
	bool ConsumeSpecifierKeyword(SpecifierState& state,
	                             bool allow_decl_specifiers);
	static bool SeenType(const SpecifierState& state);
	EFundamentalType CombineFundamental(const SpecifierState& state) const;
	TypePtr ParseTypeId();

	// --- declarators ---
	void ParseDeclarator(EDeclaratorMode mode, Declarator& out);
	void ParsePtrOperators(vector<DeclaratorChunk>& out);
	void ParseDeclaratorRoot(EDeclaratorMode mode, Declarator& out);
	void ParseDeclaratorSuffixes(vector<DeclaratorChunk>& out);
	DeclaratorChunk ParseParametersAndQualifiers();
	TypePtr ParseParameterDeclaration();
	DeclaratorChunk ParseArrayBound();
	bool LParenStartsParameters() const;
	bool ScanIsTypeName(size_t ahead) const;
	TypePtr ComputeDeclaratorType(TypePtr base, const Declarator& d) const;
	static const QualifiedName* DeclaratorName(const Declarator& d);
	unsigned long long EvaluateArrayBound(const PostToken& literal) const;

	const vector<PostToken>& tokens_;
	size_t pos_;
	PostToken eof_;
	SemaModel& model_;
	vector<Namespace*> scopes_;  // lexical namespace chain, global first
};
