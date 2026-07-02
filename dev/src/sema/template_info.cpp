#include "sema/template_info.h"

#include <cstdio>

#include "ast/ast_text.h"

using std::to_string;

namespace {

bool IdentifierChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_';
}

// Replaces whole-identifier occurrences of `name` in `text`.
void ReplaceWholeIdentifier(string& text, const string& name,
                            const string& with)
{
	if (name.empty())
		return;
	size_t at = 0;
	while ((at = text.find(name, at)) != string::npos)
	{
		bool left = at > 0 && IdentifierChar(text[at - 1]);
		bool right = at + name.size() < text.size() &&
			IdentifierChar(text[at + name.size()]);
		if (!left && !right)
		{
			text.replace(at, name.size(), with);
			at += with.size();
		}
		else
			at += name.size();
	}
}

// The first parameter clause of `declarator`, through one nesting
// level (mirrors the declarator composition's function detection).
const AstParameterClause* DeclaratorParameterClause(
	const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return item.params.get();
		if (item.kind == DI_NESTED && item.nested)
			if (const AstParameterClause* inner =
			        DeclaratorParameterClause(*item.nested))
				return inner;
	}
	return 0;
}

// Structural rendering with entity addresses: unique per distinct
// argument list within one translation unit, never printed.
void AppendKey(const TypePtr& type, string& out)
{
	if (!type)
	{
		out += "?";
		return;
	}
	if (type->is_const)
		out += "K";
	if (type->is_volatile)
		out += "V";
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		out += "f" + to_string((int)type->fundamental);
		return;
	case TK_CLASS:
	case TK_ENUM:
	case TK_TYPE_PARAM:
	case TK_MEMBER_POINTER:
	case TK_TEMPLATE_SPEC:
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "n%p", (const void*)type->named);
		out += buffer;
		break;
	}
	case TK_POINTER:
		out += "P";
		break;
	case TK_LVALUE_REFERENCE:
		out += "R";
		break;
	case TK_RVALUE_REFERENCE:
		out += "O";
		break;
	case TK_ARRAY:
		out += "A" + (type->bound_known ? to_string(type->bound)
		                                : string("?"));
		break;
	case TK_FUNCTION:
		out += "F";
		if (type->variadic)
			out += "z";
		out += to_string(type->ref_qual);
		break;
	}
	if (!type->parameters.empty())
	{
		out += "(";
		for (size_t i = 0; i < type->parameters.size(); i++)
		{
			if (i > 0)
				out += ",";
			AppendKey(type->parameters[i], out);
		}
		out += ")";
	}
	if (type->target)
	{
		out += "->";
		AppendKey(type->target, out);
	}
}

// The enclosing named namespace/class path of an argument entity.
// Distinct same-named entities must spell apart: the spellings become
// specialization entity/scope names, which the lowering's printed
// symbols and function identity keys derive from. Unnamed namespace
// components spell a stable marker for the same reason.
string EntityScopePrefix(const NamedTypeInfo& info)
{
	string path;
	for (const Scope* scope = info.scope; scope && scope->parent;
	     scope = scope->parent)
	{
		if (scope->kind != SCOPE_NAMESPACE && scope->kind != SCOPE_CLASS)
			continue;
		if (scope->name.empty())
		{
			if (scope->kind == SCOPE_NAMESPACE)
				path = "(unnamed)::" + path;
			continue;
		}
		path = scope->name + "::" + path;
	}
	return path;
}

// A source-like spelling of one type for specialization entity names.
// Only the shapes that appear in supported template-argument lists
// matter; anything else falls back to the PA7 description (which still
// sanitizes deterministically).
string ArgSpelling(const TypePtr& type)
{
	string cv;
	if (type->is_const)
		cv += "const ";
	if (type->is_volatile)
		cv += "volatile ";
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return cv + FundamentalTypeName(type->fundamental);
	case TK_CLASS:
	case TK_ENUM:
		return cv + EntityScopePrefix(*type->named) + type->named->name;
	case TK_TYPE_PARAM:
		return cv + type->named->name;
	case TK_POINTER:
		return cv + ArgSpelling(type->target) + "*";
	case TK_LVALUE_REFERENCE:
		return ArgSpelling(type->target) + "&";
	case TK_RVALUE_REFERENCE:
		return ArgSpelling(type->target) + "&&";
	case TK_ARRAY:
		return cv + ArgSpelling(type->target) + "[" +
			(type->bound_known ? to_string(type->bound) : string()) + "]";
	default:
		return cv + DescribeType(type);
	}
}

}  // namespace

// One argument's key: types render structurally; concrete values key
// by declared type + bits (14.4: type-and-value identity); pattern
// slots key by their parameter index / expression identity.
static void AppendArgKey(const TemplateArg& arg, string& out)
{
	if (!arg.is_value)
	{
		AppendKey(arg.type, out);
		return;
	}
	if (arg.value_param >= 0)
	{
		out += "vp" + to_string(arg.value_param);
		return;
	}
	if (arg.dependent_value)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "ve%p",
		         (const void*)arg.dependent_value);
		out += buffer;
		return;
	}
	out += "v";
	AppendKey(arg.type, out);
	out += ":" + to_string(arg.value_bits);
}

string TemplateArgumentKey(const vector<TemplateArg>& args)
{
	string key;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			key += ";";
		AppendArgKey(args[i], key);
	}
	return key;
}

// The source-like spelling of one concrete value: bool parameters
// spell true/false, signed types the signed decimal, everything else
// the unsigned decimal. Distinct values of one parameter cannot
// collide.
static string ValueSpelling(const TemplateArg& arg)
{
	if (arg.type && arg.type->kind == TK_FUNDAMENTAL &&
	    arg.type->fundamental == FT_BOOL)
		return arg.value_bits ? "true" : "false";
	if (IsSignedIntegralFundamental(arg.value_type))
		return to_string((long long)arg.value_bits);
	return to_string(arg.value_bits);
}

string TemplateArgumentSpelling(const vector<TemplateArg>& args)
{
	string text = "<";
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			text += ", ";
		const TemplateArg& arg = args[i];
		if (!arg.is_value)
			text += ArgSpelling(arg.type);
		else if (arg.value_param >= 0)
			text += "#v" + to_string(arg.value_param);
		else if (arg.dependent_value)
			text += "#expr";
		else
			text += ValueSpelling(arg);
	}
	text += ">";
	return text;
}

string PositionalizeTemplateNames(const string& text,
                                  const vector<TemplateParam>& params)
{
	string out = text;
	for (size_t i = 0; i < params.size(); i++)
		ReplaceWholeIdentifier(out, params[i].name,
		                       "#" + to_string(i));
	return out;
}

string CanonicalDeclaratorParams(const AstDeclarator& declarator,
                                 const vector<TemplateParam>& params)
{
	const AstParameterClause* clause =
		DeclaratorParameterClause(declarator);
	string out;
	if (clause)
		for (size_t p = 0; p < clause->parameters.size(); p++)
		{
			const AstParameter& parameter = clause->parameters[p];
			string text = FlattenSpecifierSeq(parameter.specifiers);
			if (parameter.declarator)
			{
				// The declarator-id is not part of the signature; the
				// shape (`*`, `&`, nested clauses) is.
				string shape = FlattenDeclarator(*parameter.declarator);
				const AstName* id = parameter.declarator->IdName();
				if (id && id->parts.size() == 1 &&
				    id->parts[0].kind == NP_IDENTIFIER)
					ReplaceWholeIdentifier(
						shape, id->parts[0].identifier, "");
				text += "|" + shape;
			}
			out += text + ";";
		}
	return PositionalizeTemplateNames(out, params);
}
