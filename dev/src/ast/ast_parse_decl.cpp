#include "ast/ast_parser.h"

#include <stdexcept>

using std::string;
using std::move;

namespace {

AstDeclPtr MakeDecl(EDeclKind kind)
{
	return AstDeclPtr(new AstDecl(kind));
}

// "C" from the linkage string-literal spelling "\"C\"".
string StripQuotes(const string& spelling)
{
	size_t open = spelling.find('"');
	size_t close = spelling.rfind('"');
	if (open == string::npos || close <= open)
		return spelling;
	return spelling.substr(open + 1, close - open - 1);
}

}  // namespace

AstDeclPtr AstParser::ParseTranslationUnit()
{
	AstDeclPtr unit = MakeDecl(DK_TRANSLATION_UNIT);
	for (;;)
	{
		AstDeclPtr decl = ParseDeclaration();
		if (!decl)
			break;
		unit->body_decls.push_back(move(decl));
	}
	if (!AtEof())
	{
		// Report a token neighborhood: parse failures on hosted
		// headers need the construct, not just the leading token.
		string context;
		for (size_t i = 0; i < 40 && !AtEof(); i++)
		{
			context += " " + Peek().spelling;
			Advance();
		}
		throw std::runtime_error("parse error at:" + context);
	}
	return unit;
}

// namespace-definition: KW_INLINE? KW_NAMESPACE TT_IDENTIFIER?
// { declaration* }. Named bodies use the persistent child table so a
// reopened namespace sees its earlier names and qualified lookups can
// descend into it.
AstDeclPtr AstParser::ParseNamespaceDefinition()
{
	State state = Save();
	AstDeclPtr decl = MakeDecl(DK_NAMESPACE);
	decl->inline_namespace = MatchSimple(KW_INLINE);
	if (!MatchSimple(KW_NAMESPACE))
	{
		Restore(state);
		return AstDeclPtr();
	}
	// PA34 hosted headers adorn namespaces on both sides of the name;
	// the attributes are accepted and discarded.
	while (SkipSquareAttribute())
	{
	}
	SkipDeclAdornments();
	if (AtIdentifier())
	{
		decl->name = Peek().spelling;
		Advance();
	}
	else
		decl->unnamed = true;
	while (SkipSquareAttribute())
	{
	}
	SkipDeclAdornments();
	if (!MatchSimple(OP_LBRACE))
	{
		Restore(state);
		return AstDeclPtr();
	}
	NameTable* table = decl->unnamed
		? NewTable()
		: GetOrCreateChild(DeclScopeTable(), decl->name);
	PushScope(table, false);
	while (!AtSimple(OP_RBRACE))
	{
		AstDeclPtr inner = ParseDeclaration();
		if (!inner)
		{
			// Committed once `namespace name {` matched: report the
			// failing construct instead of backtracking to a
			// context-free "parse error at token" at the file top.
			string context;
			for (size_t i = 0; i < 40 && !AtEof(); i++)
			{
				context += " " + Peek().spelling;
				Advance();
			}
			PopScope();
			throw std::runtime_error(
				"parse error in namespace body at:" + context);
		}
		decl->body_decls.push_back(move(inner));
	}
	PopScope();
	Advance();  // OP_RBRACE
	return decl;
}

// namespace-alias-definition: KW_NAMESPACE TT_IDENTIFIER OP_ASS
// qualified-id OP_SEMICOLON. The alias links to the target's table so
// alias::member can still be classified.
AstDeclPtr AstParser::ParseNamespaceAliasDefinition()
{
	State state = Save();
	if (!MatchSimple(KW_NAMESPACE) || !AtIdentifier())
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_NAMESPACE_ALIAS);
	decl->name = Peek().spelling;
	Advance();
	if (!MatchSimple(OP_ASS) || !ParseIdExpressionName(decl->target) ||
	    !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (decl->target.parts.size() == 1 &&
	    decl->target.parts[0].kind == NP_IDENTIFIER)
	{
		AstName prefix;
		prefix.global_scope = decl->target.global_scope;
		const NameTable* target = 0;
		const string& root = decl->target.parts[0].identifier;
		for (size_t i = scopes_.size(); target == 0 && i-- > 0;)
		{
			std::map<string, NameTable*>::const_iterator child =
				scopes_[i].table->children.find(root);
			if (child != scopes_[i].table->children.end())
				target = child->second;
		}
		if (target)
			DeclScopeTable()->children[decl->name] =
				const_cast<NameTable*>(target);
	}
	return decl;
}

// using-directive: KW_USING KW_NAMESPACE qualified-id ;
// using-declaration: KW_USING qualified-id ;
// Neither imports names into the table (the fixtures rely on imported
// names staying unknown).
AstDeclPtr AstParser::ParseUsingDeclarationOrDirective()
{
	State state = Save();
	if (!MatchSimple(KW_USING))
		return AstDeclPtr();
	bool directive = MatchSimple(KW_NAMESPACE);
	AstDeclPtr decl = MakeDecl(directive ? DK_USING_DIRECTIVE
	                                     : DK_USING_DECLARATION);
	// 7.3.3p1 / 14.6: `using typename Base::type;` re-exports a
	// dependent member type.
	if (!directive && MatchSimple(KW_TYPENAME))
		decl->target.typename_keyword = true;
	if (!ParseIdExpressionName(decl->target))
	{
		Restore(state);
		return AstDeclPtr();
	}
	// Clang using_if_exists: a missing target binds nothing. Other
	// trailing attributes are discarded.
	while (AtIdentifierSpelled("__attribute__"))
	{
		if (AtSimple(OP_LPAREN, 1) && AtSimple(OP_LPAREN, 2) &&
		    (Peek(3).spelling == "__using_if_exists__" ||
		     Peek(3).spelling == "using_if_exists") &&
		    AtSimple(OP_RPAREN, 4) && AtSimple(OP_RPAREN, 5))
			decl->using_if_exists = true;
		Advance();
		if (!SkipAttributeParens(0))
		{
			Restore(state);
			return AstDeclPtr();
		}
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// deduction-guide: template-head? explicit? template-name
// ( parameter-clause ) -> simple-template-id ; -- parsed and
// discarded (guides only affect class template argument deduction).
AstDeclPtr AstParser::ParseDeductionGuide()
{
	State state = Save();
	if (AtSimple(KW_TEMPLATE))
	{
		Advance();
		if (!MatchOpenAngle())
		{
			Restore(state);
			return AstDeclPtr();
		}
		PushScope(NewTable(), true);
		std::vector<AstTemplateParameter> params;
		bool ok = AtCloseAngle() || ParseTemplateParameterList(params);
		ok = ok && MatchCloseAngle();
		if (!ok)
		{
			PopScope();
			Restore(state);
			return AstDeclPtr();
		}
		if (MatchSimple(KW_EXPLICIT) && AtSimple(OP_LPAREN))
		{
			// C++20 conditional explicit-specifier on a guide.
			Advance();
			if (!ParseConditionalExpression() || !MatchSimple(OP_RPAREN))
			{
				PopScope();
				Restore(state);
				return AstDeclPtr();
			}
		}
		if (!AtIdentifier() || !AtSimple(OP_LPAREN, 1))
		{
			PopScope();
			Restore(state);
			return AstDeclPtr();
		}
		Advance();
		AstParameterClausePtr clause;
		AstTypeIdPtr type;
		if (!ParseParameterClause(clause) || !MatchSimple(OP_ARROW) ||
		    !ParseTypeId(type) || !MatchSimple(OP_SEMICOLON))
		{
			PopScope();
			Restore(state);
			return AstDeclPtr();
		}
		PopScope();
		return MakeDecl(DK_EMPTY);
	}
	if (MatchSimple(KW_EXPLICIT) && AtSimple(OP_LPAREN))
	{
		// C++20 conditional explicit-specifier on a guide.
		Advance();
		if (!ParseConditionalExpression() || !MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return AstDeclPtr();
		}
	}
	if (!AtIdentifier() || !AtSimple(OP_LPAREN, 1))
	{
		Restore(state);
		return AstDeclPtr();
	}
	Advance();
	AstParameterClausePtr clause;
	if (!ParseParameterClause(clause) || !MatchSimple(OP_ARROW))
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstTypeIdPtr type;
	if (!ParseTypeId(type) || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return MakeDecl(DK_EMPTY);
}

// alias-declaration: KW_USING TT_IDENTIFIER OP_ASS type-id ;
AstDeclPtr AstParser::ParseAliasDeclaration()
{
	State state = Save();
	if (!MatchSimple(KW_USING) || !AtIdentifier())
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_ALIAS);
	decl->name = Peek().spelling;
	Advance();
	// PA34: [[...]] attributes between the alias name and `=`.
	while (SkipSquareAttribute())
	{
	}
	if (!MatchSimple(OP_ASS))
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (!ParseTypeId(decl->type) || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	RegisterInDeclScope(decl->name, NF_TYPE);
	return decl;
}

// linkage-specification: KW_EXTERN TT_LITERAL ( { declaration* } |
// declaration ).
AstDeclPtr AstParser::ParseLinkageSpecification()
{
	State state = Save();
	if (!MatchSimple(KW_EXTERN) || !AtLiteral())
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_LINKAGE);
	decl->linkage = StripQuotes(Peek().spelling);
	Advance();
	if (MatchSimple(OP_LBRACE))
	{
		while (!AtSimple(OP_RBRACE))
		{
			AstDeclPtr inner = ParseDeclaration();
			if (!inner)
			{
				Restore(state);
				return AstDeclPtr();
			}
			decl->body_decls.push_back(move(inner));
		}
		Advance();  // OP_RBRACE
		return decl;
	}
	AstDeclPtr inner = ParseDeclaration();
	if (!inner)
	{
		Restore(state);
		return AstDeclPtr();
	}
	decl->linkage_single = true;
	decl->body_decls.push_back(move(inner));
	return decl;
}

// explicit-instantiation-declaration: KW_EXTERN KW_TEMPLATE
// (class-declaration | simple-declaration).
AstDeclPtr AstParser::ParseExplicitInstantiation()
{
	State state = Save();
	bool is_extern = MatchSimple(KW_EXTERN);  // 14.7.2: a declaration
	if (!MatchSimple(KW_TEMPLATE))
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_EXPLICIT_INSTANTIATION);
	decl->extern_instantiation = is_extern;
	decl->inner = ParseClassDeclaration();
	if (!decl->inner)
		decl->inner = ParseSimpleDeclaration();
	if (!decl->inner)
		// `extern template box<int>::box();`: a special-member form.
		decl->inner = ParseSpecialMember(false);
	if (!decl->inner)
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// static-assert-declaration: KW_STATIC_ASSERT ( assignment-expression
// (, TT_LITERAL)? ) ;
AstDeclPtr AstParser::ParseStaticAssertDeclaration()
{
	State state = Save();
	if (!MatchSimple(KW_STATIC_ASSERT) || !MatchSimple(OP_LPAREN))
	{
		Restore(state);
		return AstDeclPtr();
	}
	AstDeclPtr decl = MakeDecl(DK_STATIC_ASSERT);
	decl->assert_expr = ParseAssignmentExpression();
	if (!decl->assert_expr)
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (MatchSimple(OP_COMMA))
	{
		if (!AtLiteral())
		{
			Restore(state);
			return AstDeclPtr();
		}
		decl->has_message = true;
		decl->message = Peek().spelling;
		Advance();
	}
	if (!MatchSimple(OP_RPAREN) || !MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// simple-declaration: decl-specifier-seq init-declarator-list? ;
AstDeclPtr AstParser::ParseSimpleDeclaration()
{
	State state = Save();
	AstDeclPtr decl = MakeDecl(DK_SIMPLE);
	if (!ParseSpecifierSeq(decl->specifiers, kDeclSpecifierSeq))
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (!AtSimple(OP_SEMICOLON))
	{
		for (;;)
		{
			AstInitDeclarator init_declarator;
			init_declarator.begin_token = pos_;
			if (!ParseDeclarator(init_declarator.declarator, true))
			{
				Restore(state);
				return AstDeclPtr();
			}
			// PA34: GNU post-declarator attributes / asm labels
			// (`int x __attribute__((...)) = ...`), discarded.
			SkipDeclAdornments(&init_declarator.declarator->abi_tags);
			if (AtSimple(OP_ASS) || AtSimple(OP_LBRACE) ||
			    AtSimple(OP_LPAREN))
			{
				if (!ParseInitializer(init_declarator.init, true))
				{
					Restore(state);
					return AstDeclPtr();
				}
			}
			decl->declarators.push_back(move(init_declarator));
			if (!MatchSimple(OP_COMMA))
				break;
		}
	}
	if (!MatchSimple(OP_SEMICOLON))
	{
		Restore(state);
		return AstDeclPtr();
	}
	RegisterDeclaratorIds(*decl);
	return decl;
}

// function-definition: decl-specifier-seq declarator
// compound-statement; the declarator must be function-shaped so
// `int x{};` stays a braced-initialized simple-declaration.
namespace {

// Whether a declarator carries a parameter clause anywhere (a nested
// declarator such as `T (&f(P))[2]` hides it one level down).
bool DeclaratorHasParameterClause(const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		if (declarator.items[i].kind == DI_PARAMS)
			return true;
		if (declarator.items[i].kind == DI_NESTED &&
		    declarator.items[i].nested &&
		    DeclaratorHasParameterClause(*declarator.items[i].nested))
			return true;
	}
	return false;
}

}  // namespace

AstDeclPtr AstParser::ParseFunctionDefinition()
{
	State state = Save();
	AstDeclPtr decl = MakeDecl(DK_FUNCTION);
	if (!ParseSpecifierSeq(decl->specifiers, kDeclSpecifierSeq))
	{
		Restore(state);
		return AstDeclPtr();
	}
	if (!ParseDeclarator(decl->declarator, true))
	{
		Restore(state);
		return AstDeclPtr();
	}
	bool has_params = DeclaratorHasParameterClause(*decl->declarator);
	if (!has_params || !(AtSimple(OP_LBRACE) || AtSimple(KW_TRY)))
	{
		Restore(state);
		return AstDeclPtr();
	}
	const AstName* id = decl->declarator->IdName();
	if (id && id->IsPlainIdentifier())
		Register(id->parts[0].identifier, NF_VALUE | TemplatedFlag());
	PushTransientScope();
	RegisterParameters(*decl->declarator);
	decl->body = AtSimple(KW_TRY) ? ParseFunctionTryBody(*decl)
	                              : ParseCompoundStatement();
	PopScope();
	if (!decl->body)
	{
		Restore(state);
		return AstDeclPtr();
	}
	return decl;
}

// declaration: dispatches to the forms and stamps the parsed node with
// its terminal token span (PA11 mock names for anonymous types).
AstDeclPtr AstParser::ParseDeclaration()
{
	size_t begin = pos_;
	AstDeclPtr decl = ParseDeclarationForms();
	if (decl)
	{
		decl->begin_token = begin;
		decl->end_token = pos_;
	}
	return decl;
}

// declaration forms, ordered per the grammar with the keyword-dispatched
// forms first and the trial-parsed forms (class, enum, special
// member, function, simple) in declaration-before-expression order.
AstDeclPtr AstParser::ParseDeclarationForms()
{
	if (MatchSimple(OP_SEMICOLON))
		return MakeDecl(DK_EMPTY);
	// GNU __extension__ prefixes any declaration form (it only
	// suppresses extension diagnostics).
	if (AtIdentifierSpelled("__extension__"))
	{
		State state = Save();
		Advance();
		AstDeclPtr inner = ParseDeclarationForms();
		if (inner)
			return inner;
		Restore(state);
		return AstDeclPtr();
	}
	if (AtSimple(KW_NAMESPACE) || (AtSimple(KW_INLINE) &&
	    AtSimple(KW_NAMESPACE, 1)))
	{
		AstDeclPtr alias = ParseNamespaceAliasDefinition();
		if (alias)
			return alias;
		return ParseNamespaceDefinition();
	}
	if (AtSimple(KW_USING))
	{
		AstDeclPtr alias = ParseAliasDeclaration();
		if (alias)
			return alias;
		return ParseUsingDeclarationOrDirective();
	}
	if (AtSimple(KW_EXTERN))
	{
		if (Peek(1).kind == PTOK_LITERAL)
		{
			AstDeclPtr linkage = ParseLinkageSpecification();
			if (linkage)
				return linkage;
		}
		if (AtSimple(KW_TEMPLATE, 1))
		{
			AstDeclPtr instantiation = ParseExplicitInstantiation();
			if (instantiation)
				return instantiation;
		}
	}
	if (AtSimple(KW_TEMPLATE))
	{
		// 14.7.2: a bare `template declaration` without a parameter
		// clause is an explicit instantiation definition.
		if (!AtSimple(OP_LT, 1))
		{
			AstDeclPtr instantiation = ParseExplicitInstantiation();
			if (instantiation)
				return instantiation;
		}
		// PA34: a deduction-guide under a template head parses and
		// is accepted (class template argument deduction is a later
		// hosted stage; no guide is consulted yet).
		if (AstDeclPtr guide = ParseDeductionGuide())
			return guide;
		return ParseTemplateDeclaration();
	}
	// PA34: non-template deduction guides (`box(const char*) ->
	// box<int>;`), optionally explicit.
	if (AtIdentifier() || (AtSimple(KW_EXPLICIT) && AtIdentifier(1)))
		if (AstDeclPtr guide = ParseDeductionGuide())
			return guide;
	if (AtSimple(KW_STATIC_ASSERT))
		return ParseStaticAssertDeclaration();
	AstDeclPtr decl = ParseClassDeclaration();
	if (decl)
		return decl;
	decl = ParseEnumDeclaration();
	if (decl)
		return decl;
	decl = ParseSpecialMember(true);
	if (decl)
		return decl;
	// PA16: out-of-class defaulted special members
	// (`X::X() = default;`).
	decl = ParseSpecialMember(false, true);
	if (decl)
		return decl;
	decl = ParseFunctionDefinition();
	if (decl)
		return decl;
	return ParseSimpleDeclaration();
}
