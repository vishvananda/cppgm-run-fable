#include "ast/ast_printer.h"

#include "ast/ast_text.h"

using std::ostream;
using std::string;
using std::vector;

namespace {

void PrintDecl(const AstDecl& decl, ostream& out, int depth);
void PrintStmt(const AstStmt& stmt, ostream& out, int depth);
void PrintExpr(const AstExpr& expr, ostream& out, int depth);
void PrintExprTail(const AstExpr& expr, ostream& out, int depth);
void PrintTypeId(const AstTypeId& type, ostream& out, int depth);
void PrintDeclarator(const AstDeclarator& declarator, ostream& out,
                     int depth, const char* label);
void PrintInitializer(const AstInitializer& init, ostream& out, int depth);
void PrintTemplateParameter(const AstTemplateParameter& parameter,
                            ostream& out, int depth);

void Line(ostream& out, int depth, const string& text)
{
	for (int i = 0; i < depth; i++)
		out << "  ";
	out << text << "\n";
}

string TokenAnno(ETokenType type, const string& spelling)
{
	return TokenTypeName(type) + ":" + spelling;
}

// decl-specifier leaves: keywords keep their token annotation, a plain
// identifier type keeps the TT_IDENTIFIER form, and structured names
// (template-ids, qualified names) flatten to text with a top-level
// `typename` dropped.
void PrintDeclSpecifierSeq(const AstSpecifierSeq& seq, ostream& out, int depth)
{
	Line(out, depth, "decl-specifier-seq");
	for (size_t i = 0; i < seq.size(); i++)
	{
		const AstSpecifier& spec = seq[i];
		switch (spec.kind)
		{
		case SPEC_KEYWORD:
			Line(out, depth + 1,
			     "decl-specifier " + TokenAnno(spec.keyword, spec.spelling));
			break;
		case SPEC_TYPE_NAME:
			if (spec.name.IsPlainIdentifier())
				Line(out, depth + 1, "decl-specifier TT_IDENTIFIER:" +
				     spec.name.parts[0].identifier);
			else
				Line(out, depth + 1,
				     "decl-specifier " + FlattenNameTopLevel(spec.name));
			break;
		case SPEC_DECLTYPE:
			Line(out, depth + 1, "decl-specifier decltype(" +
			     FlattenExpr(*spec.decltype_expr) + ")");
			PrintExpr(*spec.decltype_expr, out, depth + 2);
			break;
		case SPEC_NESTED_DECL:
			PrintDecl(*spec.nested_decl, out, depth + 1);
			break;
		}
	}
}

void PrintTypeSpecifierSeq(const AstSpecifierSeq& seq, ostream& out, int depth)
{
	Line(out, depth, "type-specifier-seq");
	for (size_t i = 0; i < seq.size(); i++)
	{
		const AstSpecifier& spec = seq[i];
		switch (spec.kind)
		{
		case SPEC_KEYWORD:
			Line(out, depth + 1, (spec.IsCv() ? "cv-qualifier " : "type-specifier ") +
			     TokenAnno(spec.keyword, spec.spelling));
			break;
		case SPEC_TYPE_NAME:
			Line(out, depth + 1, "type-name " + FlattenNameTopLevel(spec.name));
			break;
		case SPEC_DECLTYPE:
			Line(out, depth + 1, "decltype-specifier decltype(" +
			     FlattenExpr(*spec.decltype_expr) + ")");
			PrintExpr(*spec.decltype_expr, out, depth + 2);
			break;
		case SPEC_NESTED_DECL:
			PrintDecl(*spec.nested_decl, out, depth + 1);
			break;
		}
	}
}

void PrintTypeId(const AstTypeId& type, ostream& out, int depth)
{
	Line(out, depth, "type-id");
	PrintTypeSpecifierSeq(type.specifiers, out, depth + 1);
	if (type.declarator && !type.declarator->Empty())
		PrintDeclarator(*type.declarator, out, depth + 1, "abstract-declarator");
}

void PrintParameterClause(const AstParameterClause& clause, ostream& out,
                          int depth)
{
	Line(out, depth, "parameter-clause");
	for (size_t i = 0; i < clause.parameters.size(); i++)
	{
		const AstParameter& parameter = clause.parameters[i];
		Line(out, depth + 1, "parameter-declaration");
		PrintDeclSpecifierSeq(parameter.specifiers, out, depth + 2);
		if (parameter.declarator && !parameter.declarator->Empty())
			PrintDeclarator(*parameter.declarator, out, depth + 2, "declarator");
		if (parameter.default_arg)
		{
			Line(out, depth + 2, "default-argument");
			PrintInitializer(*parameter.default_arg, out, depth + 3);
		}
	}
	if (clause.variadic)
		Line(out, depth + 1, "parameter-pack ...");
}

void PrintFunctionQualifier(const AstFunctionQualifier& qual, ostream& out,
                            int depth)
{
	switch (qual.kind)
	{
	case FQ_NOEXCEPT:
		if (qual.has_expr)
			Line(out, depth, "function-qualifier noexcept(" +
			     FlattenExpr(*qual.expr) + ")");
		else
			Line(out, depth, "function-qualifier noexcept");
		break;
	case FQ_THROW:
	{
		string text = "function-qualifier throw(";
		for (size_t i = 0; i < qual.throw_types.size(); i++)
		{
			if (i)
				text += ",";
			text += FlattenTypeId(*qual.throw_types[i]);
		}
		Line(out, depth, text + ")");
		break;
	}
	case FQ_VIRT:
		Line(out, depth, "function-qualifier " + qual.spelling);
		break;
	}
}

void PrintDeclarator(const AstDeclarator& declarator, ostream& out,
                     int depth, const char* label)
{
	Line(out, depth, label);
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		switch (item.kind)
		{
		case DI_PTR:
			Line(out, depth + 1, "ptr-operator " + TokenAnno(item.token, item.spelling));
			break;
		case DI_MEMBER_PTR:
			Line(out, depth + 1, "ptr-operator " + FlattenName(item.name) + "::*");
			break;
		case DI_CV:
			Line(out, depth + 1, "cv-qualifier " + TokenAnno(item.token, item.spelling));
			break;
		case DI_PACK:
			Line(out, depth + 1, "parameter-pack ...");
			break;
		case DI_ID:
			Line(out, depth + 1, "identifier " + FlattenNameTopLevel(item.name));
			break;
		case DI_NESTED:
			Line(out, depth + 1, "nested-declarator");
			PrintDeclarator(*item.nested, out, depth + 2, "declarator");
			break;
		case DI_PARAMS:
			PrintParameterClause(*item.params, out, depth + 1);
			break;
		case DI_FUNC_QUAL:
			PrintFunctionQualifier(item.qual, out, depth + 1);
			break;
		case DI_TRAILING_RETURN:
			Line(out, depth + 1, "trailing-return-type " +
			     FlattenTypeId(*item.trailing_type));
			PrintTypeId(*item.trailing_type, out, depth + 2);
			break;
		case DI_ARRAY:
			Line(out, depth + 1, "array-suffix");
			if (item.array_bound)
				PrintExpr(*item.array_bound, out, depth + 2);
			break;
		}
	}
}

void PrintInitializer(const AstInitializer& init, ostream& out, int depth)
{
	Line(out, depth, "initializer");
	switch (init.kind)
	{
	case INIT_EQ:
	case INIT_BRACED:
		PrintExpr(*init.expr, out, depth + 1);
		break;
	case INIT_PAREN:
		Line(out, depth + 1, "paren-initializer");
		for (size_t i = 0; i < init.args.size(); i++)
			PrintExpr(*init.args[i], out, depth + 2);
		break;
	case INIT_DEFAULT:
		Line(out, depth + 1, "special-initializer default");
		break;
	case INIT_DELETE:
		Line(out, depth + 1, "special-initializer delete");
		break;
	}
}

void PrintCondition(const AstCondition& condition, ostream& out, int depth)
{
	Line(out, depth, "condition");
	if (!condition.is_declaration)
	{
		PrintExpr(*condition.expr, out, depth + 1);
		return;
	}
	Line(out, depth + 1, "condition-declaration");
	PrintDeclSpecifierSeq(condition.specifiers, out, depth + 2);
	PrintDeclarator(*condition.declarator, out, depth + 2, "declarator");
	PrintInitializer(*condition.init, out, depth + 2);
}

void PrintNewExpr(const AstExpr& expr, ostream& out, int depth)
{
	Line(out, depth, "new-expression");
	if (expr.global_scope)
		Line(out, depth + 1, "global-scope");
	if (expr.has_placement)
	{
		Line(out, depth + 1,
		     "placement (" + FlattenExprList(expr.arguments) + ")");
		Line(out, depth + 2, "paren-argument-list");
		for (size_t i = 0; i < expr.arguments.size(); i++)
			PrintExpr(*expr.arguments[i], out, depth + 3);
	}
	PrintTypeId(*expr.type, out, depth + 1);
	if (expr.new_init)
		PrintInitializer(*expr.new_init, out, depth + 1);
}

void PrintLambda(const AstLambda& lambda, ostream& out, int depth)
{
	Line(out, depth, "lambda-expression");
	Line(out, depth + 1, "lambda-introducer " + FlattenLambdaIntroducer(lambda));
	if (lambda.has_declarator)
	{
		Line(out, depth + 1, "lambda-declarator");
		PrintParameterClause(*lambda.parameters, out, depth + 2);
		if (lambda.mutable_specifier)
			Line(out, depth + 2, "lambda-specifier " +
			     TokenAnno(KW_MUTABLE, lambda.mutable_spelling));
		if (lambda.has_noexcept)
		{
			Line(out, depth + 2, "noexcept-specification");
			if (lambda.noexcept_expr)
				PrintExpr(*lambda.noexcept_expr, out, depth + 3);
		}
		if (lambda.has_trailing)
		{
			Line(out, depth + 2, "trailing-return-type");
			PrintTypeId(*lambda.trailing_type, out, depth + 3);
		}
	}
	PrintStmt(*lambda.body, out, depth + 1);
}

void PrintExpr(const AstExpr& expr, ostream& out, int depth)
{
	switch (expr.kind)
	{
	case EK_LITERAL:
		Line(out, depth, "literal " + expr.literal);
		break;
	case EK_THROW:
		Line(out, depth, "throw-expression");
		for (size_t i = 0; i < expr.operands.size(); i++)
			PrintExpr(*expr.operands[i], out, depth + 1);
		break;
	case EK_STATEMENT_EXPR:
		Line(out, depth, "statement-expression");
		if (expr.stmt_body)
			PrintStmt(*expr.stmt_body, out, depth + 1);
		break;
	case EK_KEYWORD_LITERAL:
		Line(out, depth, "keyword-literal " + TokenAnno(expr.op, expr.literal));
		break;
	case EK_ID:
		Line(out, depth, "id-expression " + FlattenName(expr.name));
		break;
	case EK_PAREN:
		Line(out, depth, "parenthesized-expression");
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_BINARY:
		Line(out, depth, "binary-expression " + TokenAnno(expr.op, expr.op_spelling));
		PrintExpr(*expr.operands[0], out, depth + 1);
		PrintExpr(*expr.operands[1], out, depth + 1);
		break;
	case EK_ASSIGNMENT:
		Line(out, depth, "assignment-expression " +
		     TokenAnno(expr.op, expr.op_spelling));
		PrintExpr(*expr.operands[0], out, depth + 1);
		PrintExpr(*expr.operands[1], out, depth + 1);
		break;
	case EK_CONDITIONAL:
		Line(out, depth, "conditional-expression");
		for (size_t i = 0; i < 3; i++)
			PrintExpr(*expr.operands[i], out, depth + 1);
		break;
	case EK_UNARY:
		Line(out, depth, "unary-expression " + TokenAnno(expr.op, expr.op_spelling));
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_POSTFIX_INCDEC:
		Line(out, depth, "postfix-expression " + TokenAnno(expr.op, expr.op_spelling));
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_CALL:
		Line(out, depth, "call-expression");
		PrintExpr(*expr.operands[0], out, depth + 1);
		Line(out, depth + 1, "argument-list");
		for (size_t i = 0; i < expr.arguments.size(); i++)
			PrintExpr(*expr.arguments[i], out, depth + 2);
		break;
	case EK_FUNCTIONAL_CAST:
		Line(out, depth, "call-expression");
		Line(out, depth + 1, "id-expression " + expr.op_spelling);
		Line(out, depth + 1, "paren-argument-list");
		for (size_t i = 0; i < expr.arguments.size(); i++)
			PrintExpr(*expr.arguments[i], out, depth + 2);
		break;
	case EK_SUBSCRIPT:
		Line(out, depth, "subscript-expression");
		PrintExpr(*expr.operands[0], out, depth + 1);
		PrintExpr(*expr.operands[1], out, depth + 1);
		break;
	case EK_MEMBER:
		Line(out, depth, "member-expression " + TokenAnno(expr.op, expr.op_spelling));
		PrintExpr(*expr.operands[0], out, depth + 1);
		Line(out, depth + 1, "identifier " + FlattenName(expr.name));
		break;
	default:
		PrintExprTail(expr, out, depth);
		break;
	}
}

// The cast, sizeof, trait, allocation, closure, and list forms
// (split from PrintExpr for size).
void PrintExprTail(const AstExpr& expr, ostream& out, int depth)
{
	switch (expr.kind)
	{
	case EK_CSTYLE_CAST:
		Line(out, depth, "cast-expression " + TokenAnno(OP_LPAREN, ""));
		PrintTypeId(*expr.type, out, depth + 1);
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_KEYWORD_CAST:
		Line(out, depth, "cast-expression " + TokenAnno(expr.op, expr.op_spelling));
		PrintTypeId(*expr.type, out, depth + 1);
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_SIZEOF_EXPR:
		Line(out, depth, "sizeof-expression");
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_SIZEOF_TYPE:
		Line(out, depth, "sizeof-expression");
		PrintTypeId(*expr.type, out, depth + 1);
		break;
	case EK_SIZEOF_PACK:
		Line(out, depth, "sizeof-pack-expression " + FlattenName(expr.name));
		break;
	case EK_TYPE_TRAIT:
		Line(out, depth, "type-trait-expression " +
		     TokenAnno(expr.op, expr.op_spelling));
		if (expr.is_type_operand)
			PrintTypeId(*expr.type, out, depth + 1);
		else
			PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_BUILTIN_TRAIT:
		Line(out, depth, "builtin-trait-expression " + expr.op_spelling);
		for (size_t i = 0; i < expr.trait_args.size(); i++)
			PrintTypeId(*expr.trait_args[i].type, out, depth + 1);
		break;
	case EK_FOLD:
		Line(out, depth, "fold-expression " + expr.op_spelling);
		for (size_t i = 0; i < expr.operands.size(); i++)
			PrintExpr(*expr.operands[i], out, depth + 1);
		break;
	case EK_NEW:
		PrintNewExpr(expr, out, depth);
		break;
	case EK_DELETE:
		Line(out, depth, "delete-expression");
		if (expr.global_scope)
			Line(out, depth + 1, "global-scope");
		if (expr.array_delete)
			Line(out, depth + 1, "array-delete");
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_LAMBDA:
		PrintLambda(*expr.lambda, out, depth);
		break;
	case EK_PACK_EXPANSION:
		Line(out, depth, "pack-expansion-expression");
		PrintExpr(*expr.operands[0], out, depth + 1);
		break;
	case EK_BRACED:
		Line(out, depth, "braced-init-list");
		for (size_t i = 0; i < expr.arguments.size(); i++)
			PrintExpr(*expr.arguments[i], out, depth + 1);
		break;
	default:
		break;
	}
}

void PrintStmt(const AstStmt& stmt, ostream& out, int depth)
{
	switch (stmt.kind)
	{
	case SK_COMPOUND:
		Line(out, depth, "compound-statement");
		for (size_t i = 0; i < stmt.items.size(); i++)
			PrintStmt(*stmt.items[i], out, depth + 1);
		break;
	case SK_DECLARATION:
		PrintDecl(*stmt.decl, out, depth);
		break;
	case SK_EXPRESSION:
		Line(out, depth, "expression-statement");
		if (stmt.expr)
			PrintExpr(*stmt.expr, out, depth + 1);
		break;
	case SK_IF:
		Line(out, depth, "if-statement");
		PrintCondition(*stmt.condition, out, depth + 1);
		Line(out, depth + 1, "then");
		PrintStmt(*stmt.then_branch, out, depth + 2);
		if (stmt.else_branch)
		{
			Line(out, depth + 1, "else");
			PrintStmt(*stmt.else_branch, out, depth + 2);
		}
		break;
	case SK_SWITCH:
		Line(out, depth, "switch-statement");
		PrintCondition(*stmt.condition, out, depth + 1);
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	case SK_WHILE:
		Line(out, depth, "while-statement");
		PrintCondition(*stmt.condition, out, depth + 1);
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	case SK_DO:
		Line(out, depth, "do-statement");
		PrintStmt(*stmt.body, out, depth + 1);
		PrintCondition(*stmt.condition, out, depth + 1);
		break;
	case SK_FOR:
		Line(out, depth, "for-statement");
		Line(out, depth + 1, "for-init-statement");
		if (stmt.for_init)
			PrintStmt(*stmt.for_init, out, depth + 2);
		if (stmt.condition)
			PrintCondition(*stmt.condition, out, depth + 1);
		if (stmt.iteration)
		{
			Line(out, depth + 1, "iteration");
			PrintExpr(*stmt.iteration, out, depth + 2);
		}
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	case SK_BREAK:
		Line(out, depth, "break-statement");
		break;
	case SK_CONTINUE:
		Line(out, depth, "continue-statement");
		break;
	case SK_GOTO:
		Line(out, depth, "goto-statement " + stmt.label);
		break;
	case SK_RETURN:
		Line(out, depth, "return-statement");
		if (stmt.expr)
			PrintExpr(*stmt.expr, out, depth + 1);
		break;
	case SK_THROW:
		Line(out, depth, "throw-statement");
		if (stmt.expr)
			PrintExpr(*stmt.expr, out, depth + 1);
		break;
	case SK_TRY:
		Line(out, depth, "try-block");
		PrintStmt(*stmt.body, out, depth + 1);
		for (size_t i = 0; i < stmt.handlers.size(); i++)
		{
			const AstHandler& handler = stmt.handlers[i];
			Line(out, depth + 1, "handler");
			Line(out, depth + 2, "exception-declaration");
			if (handler.ellipsis)
				Line(out, depth + 3, "ellipsis ...");
			else
			{
				PrintDeclSpecifierSeq(handler.specifiers, out, depth + 3);
				if (handler.declarator && !handler.declarator->Empty())
					PrintDeclarator(*handler.declarator, out, depth + 3,
					                "declarator");
			}
			PrintStmt(*handler.body, out, depth + 2);
		}
		break;
	case SK_LABELED:
		Line(out, depth, "labeled-statement " + stmt.label);
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	case SK_CASE:
		Line(out, depth, "case-statement");
		PrintExpr(*stmt.expr, out, depth + 1);
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	case SK_DEFAULT:
		Line(out, depth, "default-statement");
		PrintStmt(*stmt.body, out, depth + 1);
		break;
	}
}

void PrintTemplateParameterList(const vector<AstTemplateParameter>& parameters,
                                ostream& out, int depth)
{
	Line(out, depth, "template-parameter-clause");
	if (parameters.empty())
		return;
	Line(out, depth + 1, "template-parameter-list");
	for (size_t i = 0; i < parameters.size(); i++)
		PrintTemplateParameter(parameters[i], out, depth + 2);
}

void PrintTemplateParameter(const AstTemplateParameter& parameter,
                            ostream& out, int depth)
{
	if (parameter.kind == TP_NON_TYPE)
	{
		Line(out, depth, "non-type-template-parameter");
		PrintDeclSpecifierSeq(parameter.specifiers, out, depth + 1);
		if (parameter.pack)
			Line(out, depth + 1, "parameter-pack ...");
		if (parameter.declarator && !parameter.declarator->Empty())
			PrintDeclarator(*parameter.declarator, out, depth + 1, "declarator");
		if (parameter.has_default_expr)
		{
			Line(out, depth + 1, "default-template-argument");
			// Reference-format quirk: see AstTemplateParameter.
			if (parameter.default_token_form)
				Line(out, depth + 2, "literal TT_LITERAL:" +
				     parameter.default_expr->literal);
			else
				PrintExpr(*parameter.default_expr, out, depth + 2);
		}
		return;
	}

	Line(out, depth, "type-parameter");
	if (parameter.kind == TP_TEMPLATE)
	{
		Line(out, depth + 1, "template-template-parameter");
		PrintTemplateParameterList(parameter.template_params, out, depth + 1);
	}
	Line(out, depth + 1, "parameter-key " +
	     TokenAnno(parameter.key, parameter.key_spelling));
	if (parameter.pack)
		Line(out, depth + 1, "parameter-pack ...");
	if (!parameter.name.empty())
		Line(out, depth + 1, "identifier " + parameter.name);
	if (parameter.has_default_type)
	{
		Line(out, depth + 1, "default-template-argument");
		PrintTypeId(*parameter.default_type, out, depth + 2);
	}
}

void PrintInitDeclaratorList(const vector<AstInitDeclarator>& list,
                             ostream& out, int depth)
{
	Line(out, depth, "init-declarator-list");
	for (size_t i = 0; i < list.size(); i++)
	{
		Line(out, depth + 1, "init-declarator");
		PrintDeclarator(*list[i].declarator, out, depth + 2, "declarator");
		if (list[i].init)
			PrintInitializer(*list[i].init, out, depth + 2);
	}
}

void PrintClass(const AstDecl& decl, ostream& out, int depth)
{
	string head = "class-specifier";
	if (decl.has_name)
		head += " " + FlattenNameTopLevel(decl.class_name);
	Line(out, depth, head);
	Line(out, depth + 1, "class-key " +
	     TokenAnno(decl.class_key, decl.class_key_spelling));
	if (!decl.bases.empty())
	{
		Line(out, depth + 1, "base-clause");
		for (size_t i = 0; i < decl.bases.size(); i++)
		{
			const AstBaseSpecifier& base = decl.bases[i];
			Line(out, depth + 2, "base-specifier");
			if (base.is_virtual)
				Line(out, depth + 3, "virtual " +
				     TokenAnno(KW_VIRTUAL, base.virtual_spelling));
			if (base.has_access)
				Line(out, depth + 3, "access-specifier " +
				     TokenAnno(base.access, base.access_spelling));
			Line(out, depth + 3, "base-name " + FlattenName(base.name));
			if (base.pack)
				Line(out, depth + 3, "parameter-pack ...");
		}
	}
	for (size_t i = 0; i < decl.members.size(); i++)
		PrintDecl(*decl.members[i], out, depth + 1);
}

void PrintEnum(const AstDecl& decl, ostream& out, int depth)
{
	string head = "enum-specifier";
	if (!decl.name.empty())
		head += " " + decl.name;
	Line(out, depth, head);
	if (decl.has_enum_key)
		Line(out, depth + 1, "enum-key " +
		     TokenAnno(decl.enum_key, decl.enum_key_spelling));
	if (decl.has_enum_base)
		PrintTypeId(*decl.type, out, depth + 1);
	for (size_t i = 0; i < decl.enumerators.size(); i++)
	{
		Line(out, depth + 1, "enumerator " + decl.enumerators[i].name);
		if (decl.enumerators[i].value)
			PrintExpr(*decl.enumerators[i].value, out, depth + 2);
	}
}

// Special members print their member-function-specifiers under one
// `member-specifiers` group; `explicit` prints bare (reference quirk),
// the others keep token annotations.
void PrintSpecialMember(const AstDecl& decl, ostream& out, int depth)
{
	bool definition = decl.kind == DK_SPECIAL_MEMBER_DEFINITION;
	const AstName* name = decl.declarator->IdName();
	Line(out, depth, string(definition ? "special-member-definition "
	                                   : "special-member-declaration ") +
	     (name ? FlattenNameTopLevel(*name) : string()));
	if (!decl.member_specifiers.empty())
	{
		Line(out, depth + 1, "member-specifiers");
		for (size_t i = 0; i < decl.member_specifiers.size(); i++)
		{
			const AstMemberSpecifier& spec = decl.member_specifiers[i];
			if (spec.keyword == KW_EXPLICIT)
				Line(out, depth + 2, "specifier explicit");
			else
				Line(out, depth + 2, "specifier " +
				     TokenAnno(spec.keyword, spec.spelling));
		}
	}
	PrintDeclarator(*decl.declarator, out, depth + 1, "declarator");
	if (decl.has_ctor_initializer)
	{
		Line(out, depth + 1, "ctor-initializer");
		for (size_t i = 0; i < decl.mem_initializers.size(); i++)
		{
			const AstMemInitializer& init = decl.mem_initializers[i];
			Line(out, depth + 2, "mem-initializer");
			Line(out, depth + 3, "mem-initializer-id " + FlattenName(init.id));
			if (init.init->kind == INIT_PAREN)
			{
				Line(out, depth + 3, "paren-argument-list");
				for (size_t j = 0; j < init.init->args.size(); j++)
					PrintExpr(*init.init->args[j], out, depth + 4);
			}
			else
				PrintExpr(*init.init->expr, out, depth + 3);
		}
	}
	if (decl.special_init)
		PrintInitializer(*decl.special_init, out, depth + 1);
	if (definition)
		PrintStmt(*decl.body, out, depth + 1);
}

void PrintDecl(const AstDecl& decl, ostream& out, int depth)
{
	switch (decl.kind)
	{
	case DK_TRANSLATION_UNIT:
		Line(out, depth, "translation-unit");
		for (size_t i = 0; i < decl.body_decls.size(); i++)
			PrintDecl(*decl.body_decls[i], out, depth + 1);
		break;
	case DK_EMPTY:
		Line(out, depth, "empty-declaration");
		break;
	case DK_SIMPLE:
		Line(out, depth, "simple-declaration");
		PrintDeclSpecifierSeq(decl.specifiers, out, depth + 1);
		if (!decl.declarators.empty())
			PrintInitDeclaratorList(decl.declarators, out, depth + 1);
		break;
	case DK_FUNCTION:
		Line(out, depth, "function-definition");
		PrintDeclSpecifierSeq(decl.specifiers, out, depth + 1);
		PrintDeclarator(*decl.declarator, out, depth + 1, "declarator");
		PrintStmt(*decl.body, out, depth + 1);
		break;
	case DK_NAMESPACE:
		Line(out, depth, "namespace-definition " +
		     (decl.unnamed ? string("<unnamed>") : decl.name));
		if (decl.inline_namespace)
			Line(out, depth + 1, "inline");
		for (size_t i = 0; i < decl.body_decls.size(); i++)
			PrintDecl(*decl.body_decls[i], out, depth + 1);
		break;
	case DK_NAMESPACE_ALIAS:
		Line(out, depth, "namespace-alias-definition " + decl.name);
		Line(out, depth + 1, "target " + FlattenName(decl.target));
		break;
	case DK_USING_DIRECTIVE:
		Line(out, depth, "using-directive");
		Line(out, depth + 1, "target " + FlattenName(decl.target));
		break;
	case DK_USING_DECLARATION:
		Line(out, depth, "using-declaration");
		Line(out, depth + 1, "target " + FlattenName(decl.target));
		break;
	case DK_ALIAS:
		Line(out, depth, "alias-declaration " + decl.name);
		PrintTypeId(*decl.type, out, depth + 1);
		break;
	case DK_LINKAGE:
		Line(out, depth, "linkage-specification " + decl.linkage);
		for (size_t i = 0; i < decl.body_decls.size(); i++)
			PrintDecl(*decl.body_decls[i], out, depth + 1);
		break;
	case DK_TEMPLATE:
		Line(out, depth, "template-declaration");
		PrintTemplateParameterList(decl.template_params, out, depth + 1);
		PrintDecl(*decl.inner, out, depth + 1);
		break;
	case DK_CLASS:
		PrintClass(decl, out, depth);
		break;
	case DK_CLASS_FORWARD:
		Line(out, depth, "class-forward-declaration " +
		     FlattenNameTopLevel(decl.class_name));
		Line(out, depth + 1, "class-key " +
		     TokenAnno(decl.class_key, decl.class_key_spelling));
		break;
	case DK_ENUM:
		PrintEnum(decl, out, depth);
		break;
	case DK_STATIC_ASSERT:
		Line(out, depth, "static-assert-declaration");
		PrintExpr(*decl.assert_expr, out, depth + 1);
		if (decl.has_message)
			Line(out, depth + 1, "message " + decl.message);
		break;
	case DK_ACCESS_LABEL:
		Line(out, depth, "access-specifier " +
		     TokenAnno(decl.access, decl.access_spelling));
		break;
	case DK_BIT_FIELD:
		Line(out, depth, "bit-field-declaration");
		PrintDeclSpecifierSeq(decl.specifiers, out, depth + 1);
		for (size_t i = 0; i < decl.bit_fields.size(); i++)
		{
			Line(out, depth + 1, "bit-field-declarator");
			if (decl.bit_fields[i].declarator)
				PrintDeclarator(*decl.bit_fields[i].declarator, out,
				                depth + 2, "declarator");
			PrintExpr(*decl.bit_fields[i].width, out, depth + 2);
		}
		break;
	case DK_SPECIAL_MEMBER_DECLARATION:
	case DK_SPECIAL_MEMBER_DEFINITION:
		PrintSpecialMember(decl, out, depth);
		break;
	case DK_EXPLICIT_INSTANTIATION:
		Line(out, depth, "explicit-instantiation-declaration");
		PrintDecl(*decl.inner, out, depth + 1);
		break;
	}
}

}  // namespace

void PrintAstOutput(const vector<AstDeclPtr>& units, ostream& out)
{
	out << units.size() << " translation units\n";
	for (size_t i = 0; i < units.size(); i++)
	{
		out << "start translation unit " << (i + 1) << "\n";
		PrintDecl(*units[i], out, 0);
		out << "end translation unit\n";
	}
}
