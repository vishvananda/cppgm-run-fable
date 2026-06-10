// Classes: class specifiers and heads, member declarations (with the
// bitfield FOLLOW resolution and the pure-specifier/initializer
// split), base clauses, mem-initializers, and the context-sensitive
// override/final keywords.

#include "parser.h"

using std::move;

// class-specifier: class-head OP_LBRACE member-specification* OP_RBRACE
ParseNodePtr Parser::ParseClassSpecifier()
{
	State state = Save();
	ParseNodePtr head = ParseClassHead();
	if (!head)
		return ParseNodePtr();
	if (!MatchSimple(OP_LBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("class-specifier");
	node->Add(move(head));
	for (;;)
	{
		ParseNodePtr member = ParseMemberSpecification();
		if (!member)
			break;
		node->Add(move(member));
	}
	if (!MatchSimple(OP_RBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	return node;
}

// class-head: class-key attribute-specifier* class-head-name
//     class-virt-specifier? base-clause? |
//     class-key attribute-specifier* base-clause?
// The head does not consume the `{`; reaching one validates the form.
ParseNodePtr Parser::ParseClassHead()
{
	State state = Save();
	ParseNodePtr key = ParseClassKey();
	if (!key)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("class-head");
	node->Add(move(key));
	ParseAttributeSpecifiers(node.get());
	State name_state = Save();
	ParseNodePtr name = ParseClassHeadName();
	if (name)
	{
		ParseNodePtr virt;
		if (AtIdentifier() && Peek().HasFlag(PTF_ST_FINAL))
		{
			virt = MakeParseNode("class-virt-specifier");
			virt->Add(MatchIdentifierLeaf());
		}
		ParseNodePtr base = ParseBaseClause();
		if (AtSimple(OP_LBRACE))
		{
			node->Add(move(name));
			if (virt)
				node->Add(move(virt));
			if (base)
				node->Add(move(base));
			return node;
		}
		Restore(name_state);
	}
	ParseNodePtr base = ParseBaseClause();
	if (!AtSimple(OP_LBRACE))
	{
		Restore(state);
		return ParseNodePtr();
	}
	if (base)
		node->Add(move(base));
	return node;
}

// class-head-name: nested-name-specifier? class-name
ParseNodePtr Parser::ParseClassHeadName()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("class-head-name");
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr name = ParseClassName();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	return node;
}

// class-key: KW_CLASS | KW_STRUCT | KW_UNION
ParseNodePtr Parser::ParseClassKey()
{
	if (!AtSimple(KW_CLASS) && !AtSimple(KW_STRUCT) && !AtSimple(KW_UNION))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("class-key");
	node->Add(MakeTokenLeaf(Peek()));
	Advance();
	return node;
}

// member-specification: member-declaration | access-specifier OP_COLON
ParseNodePtr Parser::ParseMemberSpecification()
{
	ParseNodePtr access = ParseAccessSpecifier();
	if (access)
	{
		State state = Save();
		if (!MatchSimple(OP_COLON))
		{
			Restore(state);
			return ParseNodePtr();
		}
		ParseNodePtr node = MakeParseNode("member-specification");
		node->Add(move(access));
		return node;
	}
	ParseNodePtr member = ParseMemberDeclaration();
	if (!member)
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("member-specification");
	node->Add(move(member));
	return node;
}

// member-declaration: attribute-specifier* decl-specifier-seq
//     member-declarator-list? OP_SEMICOLON |
//     function-definition OP_SEMICOLON? | using-declaration |
//     static_assert-declaration | template-declaration |
//     alias-declaration
ParseNodePtr Parser::ParseMemberDeclaration()
{
	ParseNodePtr node = MakeParseNode("member-declaration");
	if (AtSimple(KW_USING))
	{
		ParseNodePtr inner = ParseAliasDeclaration();
		if (!inner)
			inner = ParseUsingDeclaration();
		if (!inner)
			return ParseNodePtr();
		node->Add(move(inner));
		return node;
	}
	if (AtSimple(KW_STATIC_ASSERT))
	{
		ParseNodePtr inner = ParseStaticAssertDeclaration();
		if (!inner)
			return ParseNodePtr();
		node->Add(move(inner));
		return node;
	}
	if (AtSimple(KW_TEMPLATE))
	{
		ParseNodePtr inner = ParseTemplateDeclaration();
		if (!inner)
			return ParseNodePtr();
		node->Add(move(inner));
		return node;
	}
	State state = Save();
	ParseAttributeSpecifiers(node.get());
	ParseNodePtr specifiers = ParseDeclSpecifierSeq();
	if (specifiers)
	{
		node->Add(move(specifiers));
		if (MatchSimple(OP_SEMICOLON))
			return node;
		ParseNodePtr declarators =
			ParseCommaList("member-declarator-list",
			               &Parser::ParseMemberDeclarator);
		if (declarators && MatchSimple(OP_SEMICOLON))
		{
			node->Add(move(declarators));
			return node;
		}
		Restore(state);
	}
	else
		Restore(state);
	ParseNodePtr function = ParseFunctionDefinition();
	if (!function)
		return ParseNodePtr();
	node = MakeParseNode("member-declaration");
	node->Add(move(function));
	if (AtSimple(OP_SEMICOLON))
		node->Add(MatchSimpleLeaf(OP_SEMICOLON));
	return node;
}

// member-declarator: declarator virt-specifier* pure-specifier? |
//     declarator brace-or-equal-initializer? |
//     TT_IDENTIFIER? attribute-specifier* OP_COLON constant-expression
// The declarator readings validate against FOLLOW = {`,` `;`} so a
// named bitfield (`char b : 0?1:2;`) backs out to the bitfield form.
ParseNodePtr Parser::ParseMemberDeclarator()
{
	State state = Save();
	ParseNodePtr declarator = ParseDeclarator();
	if (declarator)
	{
		ParseNodePtr node = MakeParseNode("member-declarator");
		node->Add(move(declarator));
		bool any_virt = false;
		for (;;)
		{
			ParseNodePtr virt = ParseVirtSpecifier();
			if (!virt)
				break;
			node->Add(move(virt));
			any_virt = true;
		}
		if (AtSimple(OP_ASS) && Peek(1).HasFlag(PTF_ST_ZERO))
		{
			// pure-specifier: OP_ASS ST_ZERO (wins over an `= 0`
			// initializer per the grammar's alternative order)
			ParseNodePtr pure = MakeParseNode("pure-specifier");
			MatchSimple(OP_ASS);
			pure->Add(MatchLiteralLeaf());
			node->Add(move(pure));
		}
		else if (!any_virt)
		{
			ParseNodePtr initializer = ParseBraceOrEqualInitializer();
			if (initializer)
				node->Add(move(initializer));
		}
		if (AtSimple(OP_COMMA) || AtSimple(OP_SEMICOLON))
			return node;
		Restore(state);
	}
	// bitfield form
	ParseNodePtr node = MakeParseNode("member-declarator");
	if (AtIdentifier())
		node->Add(MatchIdentifierLeaf());
	ParseAttributeSpecifiers(node.get());
	if (!MatchSimple(OP_COLON))
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr width = ParseConstantExpression();
	if (!width)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(width));
	return node;
}

// virt-specifier: ST_OVERRIDE | ST_FINAL (context-sensitive: ordinary
// identifiers with these spellings)
ParseNodePtr Parser::ParseVirtSpecifier()
{
	if (!AtIdentifier() || !Peek().HasFlag(PTF_ST_OVERRIDE | PTF_ST_FINAL))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("virt-specifier");
	node->Add(MatchIdentifierLeaf());
	return node;
}

// base-clause: OP_COLON base-specifier-list
ParseNodePtr Parser::ParseBaseClause()
{
	State state = Save();
	if (!MatchSimple(OP_COLON))
		return ParseNodePtr();
	ParseNodePtr list = ParseCommaList("base-specifier-list",
	                                   &Parser::ParseBaseSpecifierDots);
	if (!list)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("base-clause");
	node->Add(move(list));
	return node;
}

ParseNodePtr Parser::ParseBaseSpecifierDots()
{
	return ParseDotsItem("base-specifier-dots", &Parser::ParseBaseSpecifier);
}

// base-specifier: attribute-specifier* base-type-specifier |
//     attribute-specifier* KW_VIRTUAL access-specifier?
//         base-type-specifier |
//     attribute-specifier* access-specifier KW_VIRTUAL?
//         base-type-specifier
ParseNodePtr Parser::ParseBaseSpecifier()
{
	State state = Save();
	ParseNodePtr node = MakeParseNode("base-specifier");
	ParseAttributeSpecifiers(node.get());
	if (AtSimple(KW_VIRTUAL))
	{
		node->Add(MatchSimpleLeaf(KW_VIRTUAL));
		ParseNodePtr access = ParseAccessSpecifier();
		if (access)
			node->Add(move(access));
	}
	else
	{
		ParseNodePtr access = ParseAccessSpecifier();
		if (access)
		{
			node->Add(move(access));
			if (AtSimple(KW_VIRTUAL))
				node->Add(MatchSimpleLeaf(KW_VIRTUAL));
		}
	}
	ParseNodePtr type = ParseClassOrDecltype();
	if (!type)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(type));
	return node;
}

// class-or-decltype: nested-name-specifier? class-name |
//     decltype-specifier
ParseNodePtr Parser::ParseClassOrDecltype()
{
	ParseNodePtr node = MakeParseNode("class-or-decltype");
	if (AtSimple(KW_DECLTYPE))
	{
		ParseNodePtr decltype_spec = ParseDecltypeSpecifier();
		if (!decltype_spec)
			return ParseNodePtr();
		node->Add(move(decltype_spec));
		return node;
	}
	State state = Save();
	ParseNodePtr nested = ParseNestedNameSpecifier();
	if (nested)
		node->Add(move(nested));
	ParseNodePtr name = ParseClassName();
	if (!name)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(name));
	return node;
}

// access-specifier: KW_PRIVATE | KW_PROTECTED | KW_PUBLIC
ParseNodePtr Parser::ParseAccessSpecifier()
{
	if (!AtSimple(KW_PRIVATE) && !AtSimple(KW_PROTECTED) &&
	    !AtSimple(KW_PUBLIC))
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("access-specifier");
	node->Add(MakeTokenLeaf(Peek()));
	Advance();
	return node;
}

// ctor-initializer: OP_COLON mem-initializer-list
ParseNodePtr Parser::ParseCtorInitializer()
{
	State state = Save();
	if (!MatchSimple(OP_COLON))
		return ParseNodePtr();
	ParseNodePtr list = ParseCommaList("mem-initializer-list",
	                                   &Parser::ParseMemInitializerDots);
	if (!list)
	{
		Restore(state);
		return ParseNodePtr();
	}
	ParseNodePtr node = MakeParseNode("ctor-initializer");
	node->Add(move(list));
	return node;
}

ParseNodePtr Parser::ParseMemInitializerDots()
{
	return ParseDotsItem("mem-initializer-dots",
	                     &Parser::ParseMemInitializer);
}

// mem-initializer: mem-initializer-id (OP_LPAREN expression-list?
//     OP_RPAREN | braced-init-list) with mem-initializer-id:
//     class-or-decltype | TT_IDENTIFIER
ParseNodePtr Parser::ParseMemInitializer()
{
	State state = Save();
	ParseNodePtr id = MakeParseNode("mem-initializer-id");
	ParseNodePtr name = ParseClassOrDecltype();
	if (name)
		id->Add(move(name));
	else if (AtIdentifier())
		id->Add(MatchIdentifierLeaf());
	else
		return ParseNodePtr();
	ParseNodePtr node = MakeParseNode("mem-initializer");
	node->Add(move(id));
	if (AtSimple(OP_LPAREN))
	{
		Advance();
		if (!AtSimple(OP_RPAREN))
		{
			ParseNodePtr arguments = ParseExpressionList();
			if (!arguments)
			{
				Restore(state);
				return ParseNodePtr();
			}
			node->Add(move(arguments));
		}
		if (!MatchSimple(OP_RPAREN))
		{
			Restore(state);
			return ParseNodePtr();
		}
		return node;
	}
	ParseNodePtr braced = ParseBracedInitList();
	if (!braced)
	{
		Restore(state);
		return ParseNodePtr();
	}
	node->Add(move(braced));
	return node;
}
