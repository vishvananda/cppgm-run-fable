#include "parse_tree.h"

#include <string>

using std::ostream;
using std::string;

ParseNodePtr MakeParseNode(const char* nonterminal)
{
	return ParseNodePtr(new ParseNode(nonterminal));
}

ParseNodePtr MakeTokenLeaf(const ParseToken& token)
{
	return ParseNodePtr(new ParseNode(token));
}

namespace {

// Indentation is clamped so output stays linear in the node count on
// deeply nested trees (unbounded indent makes the dump quadratic).
const int kMaxPrintIndent = 64;

void PrintNode(const ParseNode& node, ostream& out, int depth)
{
	int indent = depth < kMaxPrintIndent ? depth : kMaxPrintIndent;
	for (int i = 0; i < indent; i++)
		out << "  ";
	if (node.nonterminal)
		out << node.nonterminal << "\n";
	else
		out << "`" << node.token->spelling << "`\n";
	for (size_t i = 0; i < node.children.size(); i++)
		PrintNode(*node.children[i], out, depth + 1);
}

} // namespace

void PrintParseTree(const ParseNode& root, ostream& out)
{
	PrintNode(root, out, 0);
}
