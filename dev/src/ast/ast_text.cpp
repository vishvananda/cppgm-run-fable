#include "ast/ast_text.h"

using std::string;
using std::vector;

// Whitespace discipline (derived from the checked-in refs): qualified
// name parts join with `::` and template arguments with `,`, both with
// no spaces; specifier words join with single spaces; declarator
// tokens append without spaces except that a cv-qualifier gets one
// leading space; `typename` and `template` keywords keep one trailing
// space; expressions insert no whitespace at all.

namespace {

string FlattenNamePart(const AstNamePart& part, bool qualified);
string FlattenParameter(const AstParameter& parameter);

string FlattenTemplateArgument(const AstTemplateArgument& argument)
{
	string text;
	if (argument.is_type)
		text = FlattenTypeId(*argument.type);
	else
		text = FlattenExpr(*argument.expr);
	if (argument.pack)
		text += "...";
	return text;
}

string FlattenTemplateArguments(const vector<AstTemplateArgument>& arguments)
{
	string text = "<";
	for (size_t i = 0; i < arguments.size(); i++)
	{
		if (i)
			text += ",";
		text += FlattenTemplateArgument(arguments[i]);
	}
	text += ">";
	return text;
}

// `qualified` marks a part preceded by a nested-name-specifier; a
// qualified conversion-function-id spells `operator type` with a space
// while the unqualified form runs the words together (per the refs).
string FlattenNamePart(const AstNamePart& part, bool qualified)
{
	string text;
	if (part.template_keyword)
		text += "template ";
	if (part.tilde)
		text += "~";
	switch (part.kind)
	{
	case NP_IDENTIFIER:
		text += part.identifier;
		break;
	case NP_TEMPLATE_ID:
		text += part.identifier;
		text += FlattenTemplateArguments(part.arguments);
		break;
	case NP_OPERATOR_FUNCTION:
		text += "operator" + part.operator_text;
		break;
	case NP_CONVERSION_FUNCTION:
		text += qualified ? "operator " : "operator";
		text += FlattenTypeId(*part.conversion_type);
		break;
	case NP_LITERAL_OPERATOR:
		text += "operator\"\"" + part.identifier;
		break;
	case NP_DECLTYPE:
		text += "decltype(" + FlattenExpr(*part.decltype_expr) + ")";
		break;
	}
	return text;
}

string FlattenNameBody(const AstName& name)
{
	string text;
	if (name.global_scope)
		text += "::";
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		if (i)
			text += "::";
		text += FlattenNamePart(name.parts[i], i > 0);
	}
	return text;
}

string FlattenFunctionQualifier(const AstFunctionQualifier& qual)
{
	switch (qual.kind)
	{
	case FQ_NOEXCEPT:
		if (qual.has_expr)
			return "noexcept(" + FlattenExpr(*qual.expr) + ")";
		return "noexcept";
	case FQ_THROW:
	{
		string text = "throw(";
		for (size_t i = 0; i < qual.throw_types.size(); i++)
		{
			if (i)
				text += ",";
			text += FlattenTypeId(*qual.throw_types[i]);
		}
		text += ")";
		return text;
	}
	case FQ_VIRT:
		return qual.spelling;
	}
	return string();
}

string FlattenParameterClause(const AstParameterClause& clause)
{
	string text = "(";
	for (size_t i = 0; i < clause.parameters.size(); i++)
	{
		if (i)
			text += ",";
		text += FlattenParameter(clause.parameters[i]);
	}
	if (clause.variadic)
	{
		if (!clause.parameters.empty())
			text += ",";
		text += "...";
	}
	text += ")";
	return text;
}

string FlattenParameter(const AstParameter& parameter)
{
	string text = FlattenSpecifierSeq(parameter.specifiers);
	if (parameter.declarator)
		text += FlattenDeclarator(*parameter.declarator);
	return text;
}

string FlattenNewExpr(const AstExpr& expr)
{
	string text;
	if (expr.global_scope)
		text += "::";
	text += "new";
	if (expr.has_placement)
		text += "(" + FlattenExprList(expr.arguments) + ")";
	if (expr.paren_type)
		text += "(" + FlattenTypeId(*expr.type) + ")";
	else
		text += FlattenTypeId(*expr.type);
	if (expr.new_init)
	{
		const AstInitializer& init = *expr.new_init;
		if (init.kind == INIT_PAREN)
			text += "(" + FlattenExprList(init.args) + ")";
		else if (init.expr)
			text += FlattenExpr(*init.expr);
	}
	return text;
}

}  // namespace

string FlattenName(const AstName& name)
{
	string text;
	if (name.typename_keyword)
		text += "typename ";
	text += FlattenNameBody(name);
	return text;
}

string FlattenNameTopLevel(const AstName& name)
{
	return FlattenNameBody(name);
}

string FlattenLambdaIntroducer(const AstLambda& lambda)
{
	string text = "[";
	if (lambda.has_capture_default)
		text += lambda.capture_default == OP_AMP ? "&" : "=";
	for (size_t i = 0; i < lambda.captures.size(); i++)
	{
		if (i || lambda.has_capture_default)
			text += ",";
		const AstLambdaCapture& capture = lambda.captures[i];
		switch (capture.kind)
		{
		case LC_THIS:
			text += "this";
			break;
		case LC_COPY:
			text += capture.identifier;
			break;
		case LC_REF:
			text += "&" + capture.identifier;
			break;
		}
		if (capture.pack)
			text += "...";
	}
	return text + "]";
}

string FlattenSpecifier(const AstSpecifier& specifier)
{
	switch (specifier.kind)
	{
	case SPEC_KEYWORD:
		return specifier.spelling;
	case SPEC_TYPE_NAME:
		return FlattenName(specifier.name);
	case SPEC_DECLTYPE:
		return "decltype(" + FlattenExpr(*specifier.decltype_expr) + ")";
	case SPEC_NESTED_DECL:
	{
		const AstDecl& decl = *specifier.nested_decl;
		string text = decl.kind == DK_ENUM ? "enum" : decl.class_key_spelling;
		if (decl.has_name)
			text += " " + FlattenName(decl.class_name);
		else if (!decl.name.empty())
			text += " " + decl.name;
		return text;
	}
	}
	return string();
}

string FlattenSpecifierSeq(const AstSpecifierSeq& specifiers)
{
	string text;
	for (size_t i = 0; i < specifiers.size(); i++)
	{
		if (i)
			text += " ";
		text += FlattenSpecifier(specifiers[i]);
	}
	return text;
}

string FlattenDeclarator(const AstDeclarator& declarator)
{
	string text;
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		switch (item.kind)
		{
		case DI_PTR:
			text += item.spelling;
			break;
		case DI_MEMBER_PTR:
			text += FlattenName(item.name) + "::*";
			break;
		case DI_CV:
			text += " " + item.spelling;
			break;
		case DI_PACK:
			text += "...";
			break;
		case DI_ID:
			text += FlattenNameTopLevel(item.name);
			break;
		case DI_NESTED:
			text += "(" + FlattenDeclarator(*item.nested) + ")";
			break;
		case DI_PARAMS:
			text += FlattenParameterClause(*item.params);
			break;
		case DI_FUNC_QUAL:
			text += FlattenFunctionQualifier(item.qual);
			break;
		case DI_TRAILING_RETURN:
			text += "->" + FlattenTypeId(*item.trailing_type);
			break;
		case DI_ARRAY:
			text += "[";
			if (item.array_bound)
				text += FlattenExpr(*item.array_bound);
			text += "]";
			break;
		}
	}
	return text;
}

string FlattenTypeId(const AstTypeId& type)
{
	string text = FlattenSpecifierSeq(type.specifiers);
	if (type.declarator)
		text += FlattenDeclarator(*type.declarator);
	return text;
}

string FlattenExprList(const vector<AstExprPtr>& list)
{
	string text;
	for (size_t i = 0; i < list.size(); i++)
	{
		if (i)
			text += ",";
		text += FlattenExpr(*list[i]);
	}
	return text;
}

string FlattenExpr(const AstExpr& expr)
{
	switch (expr.kind)
	{
	case EK_LITERAL:
	case EK_KEYWORD_LITERAL:
		return expr.literal;
	case EK_ID:
		return FlattenName(expr.name);
	case EK_PAREN:
		return "(" + FlattenExpr(*expr.operands[0]) + ")";
	case EK_STATEMENT_EXPR:
		return "({...})";
	case EK_THROW:
		return expr.operands.empty()
			? string("throw")
			: "throw " + FlattenExpr(*expr.operands[0]);
	case EK_BINARY:
	case EK_ASSIGNMENT:
		return FlattenExpr(*expr.operands[0]) + expr.op_spelling +
			FlattenExpr(*expr.operands[1]);
	case EK_CONDITIONAL:
		return FlattenExpr(*expr.operands[0]) + "?" +
			FlattenExpr(*expr.operands[1]) + ":" +
			FlattenExpr(*expr.operands[2]);
	case EK_UNARY:
		return expr.op_spelling + FlattenExpr(*expr.operands[0]);
	case EK_POSTFIX_INCDEC:
		return FlattenExpr(*expr.operands[0]) + expr.op_spelling;
	case EK_CALL:
		return FlattenExpr(*expr.operands[0]) + "(" +
			FlattenExprList(expr.arguments) + ")";
	case EK_FUNCTIONAL_CAST:
		return expr.op_spelling + "(" + FlattenExprList(expr.arguments) + ")";
	case EK_SUBSCRIPT:
		return FlattenExpr(*expr.operands[0]) + "[" +
			FlattenExpr(*expr.operands[1]) + "]";
	case EK_MEMBER:
		return FlattenExpr(*expr.operands[0]) + expr.op_spelling +
			FlattenName(expr.name);
	case EK_CSTYLE_CAST:
		return "(" + FlattenTypeId(*expr.type) + ")" +
			FlattenExpr(*expr.operands[0]);
	case EK_KEYWORD_CAST:
		return expr.op_spelling + "<" + FlattenTypeId(*expr.type) + ">(" +
			FlattenExpr(*expr.operands[0]) + ")";
	case EK_SIZEOF_EXPR:
		if (expr.sizeof_paren)
			return "sizeof(" + FlattenExpr(*expr.operands[0]) + ")";
		return "sizeof " + FlattenExpr(*expr.operands[0]);
	case EK_SIZEOF_TYPE:
		return "sizeof(" + FlattenTypeId(*expr.type) + ")";
	case EK_SIZEOF_PACK:
		return "sizeof...(" + FlattenName(expr.name) + ")";
	case EK_TYPE_TRAIT:
		if (expr.is_type_operand)
			return expr.op_spelling + "(" + FlattenTypeId(*expr.type) + ")";
		return expr.op_spelling + "(" + FlattenExpr(*expr.operands[0]) + ")";
	case EK_FOLD:
	{
		string text = "(";
		if (expr.fold_pack_first || expr.operands.size() == 2)
			text += FlattenExpr(*expr.operands[0]) + " " +
				expr.op_spelling + " ";
		text += "...";
		if (!expr.fold_pack_first || expr.operands.size() == 2)
			text += " " + expr.op_spelling + " " +
				FlattenExpr(*expr.operands[expr.operands.size() - 1]);
		return text + ")";
	}
	case EK_BUILTIN_TRAIT:
	{
		string text = expr.op_spelling + "(";
		for (size_t i = 0; i < expr.trait_args.size(); i++)
		{
			if (i)
				text += ", ";
			text += FlattenTypeId(*expr.trait_args[i].type);
			if (expr.trait_args[i].pack)
				text += "...";
		}
		return text + ")";
	}
	case EK_NEW:
		return FlattenNewExpr(expr);
	case EK_DELETE:
	{
		string text = expr.global_scope ? "::delete" : "delete";
		if (expr.array_delete)
			text += "[]";
		return text + " " + FlattenExpr(*expr.operands[0]);
	}
	case EK_LAMBDA:
		return FlattenLambdaIntroducer(*expr.lambda);
	case EK_PACK_EXPANSION:
		return FlattenExpr(*expr.operands[0]) + "...";
	case EK_BRACED:
		return "{" + FlattenExprList(expr.arguments) + "}";
	}
	return string();
}
