#include "sema/sem_node.h"

#include <stdexcept>

using std::runtime_error;

const char* ValueCategoryName(EValueCategory category)
{
	switch (category)
	{
	case VC_LVALUE: return "lvalue";
	case VC_XVALUE: return "xvalue";
	case VC_PRVALUE: break;
	}
	return "prvalue";
}

SemNode::SemNode(ESemNodeKind kind_in)
	: kind(kind_in), category(VC_PRVALUE), has_op(false), op(OP_STAR),
	  entity_scope(0), has_value(false), null_pointer(false),
	  is_string_literal(false), is_static_decl(false),
	  is_extern_decl(false), is_thread_local_decl(false),
	  c_linkage(false), unwind_no(false), member_offset(0),
	  base_hops(0), is_bit_field(false), bit_offset(0), bit_width(0),
	  member_ref(false), is_method(false), special(SF_NONE),
	  inline_def(false), needs_dtor(false), trivial_init(false),
	  elided(false), bf_plain_store(false), synth_copy(false),
	  trivial_copy(false), ctor_addressed(false), synthesized(false)
{
}

SemNodePtr MakeSemNode(ESemNodeKind kind)
{
	return SemNodePtr(new SemNode(kind));
}

namespace {

// The fixed line keyword of each node kind; the variable parts are
// appended by NodeLine.
const char* NodeKeyword(ESemNodeKind kind)
{
	switch (kind)
	{
	case SN_TYPE_ALIAS: return "type-alias";
	case SN_VARIABLE: return "variable";
	case SN_FUNCTION_DECLARATION: return "function-declaration";
	case SN_FUNCTION_DEFINITION: return "function-definition";
	case SN_NAMESPACE_DEFINITION: return "namespace-definition";
	case SN_PARAMETER: return "parameter";
	case SN_CONSTRUCTOR_ACTION: return "constructor-action";
	case SN_DESTRUCTOR_ACTION: return "destructor-action";
	case SN_COMPOUND_STATEMENT: return "compound-statement";
	case SN_SIMPLE_DECLARATION: return "simple-declaration";
	case SN_EXPRESSION_STATEMENT: return "expression-statement";
	case SN_RETURN_STATEMENT: return "return-statement";
	case SN_IF_STATEMENT: return "if-statement";
	case SN_THEN: return "then";
	case SN_ELSE: return "else";
	case SN_WHILE_STATEMENT: return "while-statement";
	case SN_DO_STATEMENT: return "do-statement";
	case SN_FOR_STATEMENT: return "for-statement";
	case SN_FOR_INIT: return "for-init-statement";
	case SN_ITERATION: return "iteration";
	case SN_CONDITION: return "condition";
	case SN_CONDITION_DECLARATION: return "condition-declaration";
	case SN_SWITCH_STATEMENT: return "switch-statement";
	case SN_CASE_STATEMENT: return "case-statement";
	case SN_DEFAULT_STATEMENT: return "default-statement";
	case SN_BREAK_STATEMENT: return "break-statement";
	case SN_CONTINUE_STATEMENT: return "continue-statement";
	case SN_GOTO_STATEMENT: return "goto-statement";
	case SN_LABEL_STATEMENT: return "label-statement";
	case SN_LITERAL: return "literal";
	case SN_ID_EXPRESSION: return "id-expression";
	case SN_CALL_EXPRESSION: return "call-expression";
	case SN_CALLEE: return "callee";
	case SN_UNARY_EXPRESSION: return "unary-expression";
	case SN_BINARY_EXPRESSION: return "binary-expression";
	case SN_POSTFIX_EXPRESSION: return "postfix-expression";
	case SN_ASSIGNMENT_EXPRESSION: return "assignment-expression";
	case SN_CONDITIONAL_EXPRESSION: return "conditional-expression";
	case SN_SUBSCRIPT_EXPRESSION: return "subscript-expression";
	case SN_MEMBER_EXPRESSION: return "member-expression";
	case SN_CAST_EXPRESSION: return "cast-expression";
	case SN_SIZEOF_EXPRESSION: return "sizeof-expression";
	case SN_BRACED_INIT_LIST: return "braced-init-list";
	case SN_STORAGE_COPY: return "storage-copy";
	case SN_NEW_INIT: return "new-init";
	case SN_NEW_ARRAY: return "new-array";
	case SN_DELETE_EXPRESSION: return "delete-expression";
	case SN_DELETE_ARRAY: return "delete-array";
	}
	throw runtime_error("unknown semantic node kind");
}

string OpAnnotation(const SemNode& node)
{
	return TokenTypeName(node.op) + ":" + node.op_spelling;
}

string NodeLine(const SemNode& node)
{
	string line = NodeKeyword(node.kind);
	switch (node.kind)
	{
	case SN_TYPE_ALIAS:
	case SN_VARIABLE:
	case SN_FUNCTION_DECLARATION:
	case SN_FUNCTION_DEFINITION:
	case SN_PARAMETER:
	case SN_CALLEE:
		return line + " " + node.name + " " + DescribeType(node.type);
	case SN_NAMESPACE_DEFINITION:
	case SN_CONSTRUCTOR_ACTION:
	case SN_GOTO_STATEMENT:
	case SN_LABEL_STATEMENT:
		return line + " " + node.name;
	case SN_LITERAL:
		return line + " " + ValueCategoryName(node.category) + " " +
			DescribeType(node.type) + " " + node.token;
	case SN_ID_EXPRESSION:
		return line + " " + ValueCategoryName(node.category) + " " +
			DescribeType(node.type) + " " + node.name;
	case SN_UNARY_EXPRESSION:
	case SN_BINARY_EXPRESSION:
	case SN_POSTFIX_EXPRESSION:
	case SN_ASSIGNMENT_EXPRESSION:
		return line + " " + ValueCategoryName(node.category) + " " +
			DescribeType(node.type) + " " + OpAnnotation(node);
	case SN_MEMBER_EXPRESSION:
		// Explicit accesses annotate the operator token; injected
		// anonymous-union member accesses print the bare member name.
		return line + " " + ValueCategoryName(node.category) + " " +
			DescribeType(node.type) + " " +
			(node.has_op ? TokenTypeName(node.op) + ":" : "") + node.name;
	case SN_CAST_EXPRESSION:
		line += string(" ") + ValueCategoryName(node.category) + " " +
			DescribeType(node.type);
		if (node.has_op)
			line += " " + OpAnnotation(node);
		return line;
	case SN_CALL_EXPRESSION:
	case SN_CONDITIONAL_EXPRESSION:
	case SN_SUBSCRIPT_EXPRESSION:
	case SN_SIZEOF_EXPRESSION:
	case SN_BRACED_INIT_LIST:
		return line + " " + ValueCategoryName(node.category) + " " +
			DescribeType(node.type);
	default:
		return line;  // statements
	}
}

void PrintNode(const SemNode& node, ostream& out, int depth)
{
	if (node.kind == SN_CONSTRUCTOR_ACTION && node.synth_copy)
	{
		// A PA16 copy/move-initialization wrapper: the PA12 dump pins
		// the wrapped source expression (the call's last child).
		const SemNode& call = *node.children[0];
		PrintNode(*call.children.back(), out, depth);
		return;
	}
	for (int i = 0; i < depth; i++)
		out << "  ";
	out << NodeLine(node) << "\n";
	for (size_t i = 0; i < node.children.size(); i++)
		PrintNode(*node.children[i], out, depth + 1);
}

}  // namespace

void PrintSemanticsOutput(const SemUnit& unit, ostream& out)
{
	out << "translation-unit\n";
	for (size_t i = 0; i < unit.items.size(); i++)
		PrintNode(*unit.items[i], out, 1);
	for (size_t i = 0; i < unit.synthesized.size(); i++)
		PrintNode(*unit.synthesized[i], out, 1);
}

SemNodePtr CloneSemNode(const SemNode& node)
{
	SemNodePtr copy = MakeSemNode(node.kind);
	SemNode& out = *copy;
	out.name = node.name;
	out.type = node.type;
	out.category = node.category;
	out.has_op = node.has_op;
	out.op = node.op;
	out.op_spelling = node.op_spelling;
	out.token = node.token;
	out.entity_scope = node.entity_scope;
	out.entity_name = node.entity_name;
	out.has_value = node.has_value;
	out.value = node.value;
	out.null_pointer = node.null_pointer;
	out.is_string_literal = node.is_string_literal;
	out.string_bytes = node.string_bytes;
	out.is_static_decl = node.is_static_decl;
	out.is_extern_decl = node.is_extern_decl;
	out.is_thread_local_decl = node.is_thread_local_decl;
	out.c_linkage = node.c_linkage;
	out.unwind_no = node.unwind_no;
	out.member_offset = node.member_offset;
	out.base_hops = node.base_hops;
	out.is_bit_field = node.is_bit_field;
	out.bit_offset = node.bit_offset;
	out.bit_width = node.bit_width;
	out.is_method = node.is_method;
	out.special = node.special;
	out.inline_def = node.inline_def;
	out.needs_dtor = node.needs_dtor;
	out.trivial_init = node.trivial_init;
	out.elided = node.elided;
	out.bf_plain_store = node.bf_plain_store;
	out.member_ref = node.member_ref;
	out.synth_copy = node.synth_copy;
	out.trivial_copy = node.trivial_copy;
	out.ctor_addressed = node.ctor_addressed;
	out.synthesized = node.synthesized;
	if (node.result_dtor)
		out.result_dtor = CloneSemNode(*node.result_dtor);
	for (size_t i = 0; i < node.children.size(); i++)
		out.children.push_back(CloneSemNode(*node.children[i]));
	return copy;
}
