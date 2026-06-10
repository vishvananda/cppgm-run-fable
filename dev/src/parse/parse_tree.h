#pragma once

#include <memory>
#include <ostream>
#include <vector>

#include "parse_token.h"

// The PA6 placeholder parse tree (the handout's dynamic AST): a
// successful parse function returns a node tagged with its nonterminal
// name whose ordered children are sub-parses and consumed-token
// leaves. The tree only needs to witness the recognized structure for
// debugging output; later assignments replace it with typed AST
// classes built during semantic analysis.
struct ParseNode;
typedef std::unique_ptr<ParseNode> ParseNodePtr;

struct ParseNode
{
	explicit ParseNode(const char* nonterminal_in)
		: nonterminal(nonterminal_in), token(0)
	{}

	explicit ParseNode(const ParseToken& token_in)
		: nonterminal(0), token(&token_in)
	{}

	void Add(ParseNodePtr child)
	{
		children.push_back(std::move(child));
	}

	const char* nonterminal;          // null for token leaves
	const ParseToken* token;          // null for nonterminal nodes;
	                                  // points into the parsed sequence
	std::vector<ParseNodePtr> children;
};

ParseNodePtr MakeParseNode(const char* nonterminal);
ParseNodePtr MakeTokenLeaf(const ParseToken& token);

// Renders the indented debugging tree (free-form output; the PA6
// harness gates only the outfile and exit status).
void PrintParseTree(const ParseNode& root, std::ostream& out);
