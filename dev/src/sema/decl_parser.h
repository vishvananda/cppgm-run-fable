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
#include "sema/expr.h"
#include "sema/name_lookup.h"

class Program;

// PA7/PA8 semantic parser: one forward recursive-descent pass over the
// pa8.gram translation-unit grammar (a superset of pa7.gram) that
// performs the semantic actions inline - 7.3 namespace forms,
// declaration matching and linking, type composition per clause 8,
// expression annotation and initialization (clauses 4, 5.19, 8.5) -
// populating the per-TU SemaModel and the program-wide Program. It
// consumes the phase-7 PostToken sequence directly because literals
// need their type and value bytes.
//
// The grammar's choice points are all resolved predictively from typed
// lookup state, never by backtracking over semantic actions:
//  - declaration alternatives by one or two tokens of lookahead;
//  - an identifier in specifier position is a type-name exactly when
//    no type-specifier has been seen yet (every pa8 declaration has
//    exactly one type);
//  - '(' inside an abstract-capable declarator starts a parameter
//    clause or a parenthesized declarator per the 8.2p7 rule, deciding
//    by the following token and, for identifiers, by whether the
//    qualified name resolves to a typedef (a positional scan with no
//    side effects);
//  - a '{' after the first declarator makes the declaration a
//    function-definition.
//
// Diagnosable ill-formed programs throw runtime_error (the tool exits
// EXIT_FAILURE, which PA8 requires); inputs that do not match pa8.gram
// are undefined behaviour and throw too.
class DeclParser
{
public:
	DeclParser(const vector<PostToken>& tokens, SemaModel& model,
	           Program& program);

	// Parses the whole token sequence, building the model's namespaces
	// and the program's entities.
	void ParseTranslationUnit();

private:
	// An optionally qualified name: the nested-name-specifier components
	// (each a namespace-name) and the final unqualified-id.
	struct QualifiedName
	{
		QualifiedName() : root_global(false), scope(0) {}

		bool Qualified() const
		{
			return root_global || !path.empty();
		}

		bool root_global;      // leading ::
		vector<string> path;
		string name;
		// For a qualified declarator-id: the namespace the path names
		// (resolved at parse time, when the lookup scope switches).
		Namespace* scope;
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

	enum ESpecifierContext
	{
		SC_DECLARATION,  // decl-specifiers allowed
		SC_PARAMETER,    // type-specifiers only (8.3.5)
		SC_TYPE_ID       // type-specifiers only
	};

	struct DeclSpecifiers
	{
		DeclSpecifiers()
			: is_typedef(false), is_static(false), is_thread_local(false),
			  is_extern(false), is_constexpr(false), is_inline(false)
		{}

		TypePtr type;
		bool is_typedef;
		bool is_static;
		bool is_thread_local;
		bool is_extern;
		bool is_constexpr;
		bool is_inline;
	};

	// Accumulated decl-specifier-seq facts before combination per the
	// 7.1.6.2 table and the 7.1.1 storage-class rules.
	struct SpecifierState
	{
		SpecifierState() : signed_count(0), unsigned_count(0),
		                   short_count(0), long_count(0), has_base(false),
		                   base(KW_INT), is_const(false), is_volatile(false),
		                   is_typedef(false), is_static(false),
		                   is_thread_local(false), is_extern(false),
		                   is_constexpr(false), is_inline(false) {}

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
		bool is_static;
		bool is_thread_local;
		bool is_extern;
		bool is_constexpr;
		bool is_inline;
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
	void ParseStaticAssert();
	void ParseSimpleDeclaration();
	void ParseFunctionDefinition(const DeclSpecifiers& specs,
	                             const Declarator& declarator);
	DeclaredEntity* DeclareSimple(const DeclSpecifiers& specs,
	                              const Declarator& declarator);
	DeclaredEntity* DeclareQualified(const DeclSpecifiers& specs,
	                                 const QualifiedName& name,
	                                 const TypePtr& type);
	DeclaredEntity* DeclareVariable(const DeclSpecifiers& specs,
	                                const string& name, const TypePtr& type);
	DeclaredEntity* DeclareFunction(const DeclSpecifiers& specs,
	                                const string& name, const TypePtr& type);
	DeclaredEntity* LinkNewEntity(const string& name, const TypePtr& type,
	                              ESlotKind entity_kind,
	                              const DeclSpecifiers& specs);
	// Redeclaration of `entity` with `specs`: linkage consistency
	// (7.1.1p7) and the constexpr agreement rule (7.1.5p2).
	void CheckRedeclarationSpecifiers(DeclaredEntity* entity,
	                                  const DeclSpecifiers& specs) const;
	void FinishUninitializedDeclaration(const DeclSpecifiers& specs,
	                                    DeclaredEntity* entity);
	void BindTypedef(const string& name, const TypePtr& type);

	// --- expressions ---
	Expr ParseExpression();
	Expr AnnotateIdExpression(const QualifiedName& name);

	// --- names ---
	QualifiedName ParseIdExpression();
	Namespace* ParseNamespaceSpecifier();
	TypePtr ResolveTypeName(const QualifiedName& name) const;
	// Resolves the namespace a nested-name-specifier path names; null
	// when any component fails to resolve.
	Namespace* ResolveNamespacePath(bool root_global,
	                                const vector<string>& path) const;
	// Resolves an optionally qualified name (false when any step
	// fails): unqualified lookup for a lone identifier, otherwise the
	// nested-name-specifier chain then qualified lookup of `last`.
	bool ResolveComponents(bool root_global, const vector<string>& path,
	                       const string& last, ELookupFilter filter,
	                       Binding& out) const;

	// --- specifiers ---
	DeclSpecifiers ParseDeclSpecifierSeq(ESpecifierContext context);
	bool ConsumeSpecifierKeyword(SpecifierState& state,
	                             ESpecifierContext context);
	static bool SeenType(const SpecifierState& state);
	EFundamentalType CombineFundamental(const SpecifierState& state) const;
	TypePtr ParseTypeId();

	// --- declarators ---
	void ParseDeclarator(EDeclaratorMode mode, Declarator& out);
	void ParsePtrOperators(vector<DeclaratorChunk>& out);
	void ParseDeclaratorRoot(EDeclaratorMode mode, Declarator& out);
	// Resolves a qualified declarator-id's namespace, checks 8.3p1
	// enclosure, and switches the lookup scope chain (3.4.3p3) until
	// RestoreDeclaratorScope (a no-op when no switch happened).
	void EnterQualifiedDeclaratorScope(QualifiedName& name);
	void RestoreDeclaratorScope();
	void ParseDeclaratorSuffixes(vector<DeclaratorChunk>& out);
	DeclaratorChunk ParseParametersAndQualifiers();
	TypePtr ParseParameterDeclaration();
	DeclaratorChunk ParseArrayBound();
	bool LParenStartsParameters() const;
	bool ScanIsTypeName(size_t ahead) const;
	TypePtr ComputeDeclaratorType(const TypePtr& base,
	                              const Declarator& declarator) const;
	TypePtr ComputeDeclaratorTypeRec(TypePtr type,
	                                 const Declarator& declarator,
	                                 const TypePtr& spec_base) const;
	static const QualifiedName* DeclaratorName(const Declarator& d);
	// True when any level of the declarator has a type chunk (a
	// chunk-free declarator's type is the decl-specifier-seq type).
	static bool HasDeclaratorChunks(const Declarator& d);

	const vector<PostToken>& tokens_;
	size_t pos_;
	PostToken eof_;
	SemaModel& model_;
	Program& program_;
	vector<Namespace*> scopes_;  // lexical namespace chain, global first
	// The lexical chain saved by EnterQualifiedDeclaratorScope (swapped,
	// not copied, so unqualified declarators cost nothing).
	vector<Namespace*> saved_scopes_;
	bool scope_switched_;

	// Directive-closure memo for UnqualifiedLookup; invalidated at the
	// two places that add using-directives (ParseUsingDirective and
	// namespace creation, whose unnamed/inline forms add the implicit
	// directive). Mutable: lookups are logically const.
	mutable DirectiveClosureCache lookup_cache_;
};
