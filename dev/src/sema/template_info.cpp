#include "sema/template_info.h"

#include <cstdio>

#include "ast/ast_text.h"

using std::to_string;

static void AppendArgKey(const struct TemplateArg& arg, string& out);

namespace {

void AppendArgKeyRef(const struct TemplateArg& arg, string& out)
{
	AppendArgKey(arg, out);
}

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
		// PA21: a dependent specialization pattern's identity includes
		// its argument pattern (distinct `typelist<A>` / `typelist<A,
		// B>` patterns must not collide).
		if (type->kind == TK_TEMPLATE_SPEC)
		{
			out += "<";
			for (size_t i = 0; i < type->targs.size(); i++)
			{
				if (i)
					out += ",";
				AppendArgKeyRef(type->targs[i], out);
			}
			out += ">";
		}
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
// (AppendArgKeyRef is defined after AppendArgKey below.)
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
	case TK_FUNCTION:
	{
		string parameters;
		for (size_t i = 0; i < type->parameters.size(); i++)
		{
			if (i > 0)
				parameters += ", ";
			parameters += ArgSpelling(type->parameters[i]);
		}
		if (type->variadic)
			parameters += type->parameters.empty() ? "..." : ", ...";
		return cv + ArgSpelling(type->target) + "(" + parameters + ")";
	}
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
	if (arg.pack_pattern)
		out += "pk";
	if (arg.dependent_type)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "dt%p",
		         (const void*)arg.dependent_type);
		out += buffer;
		return;
	}
	// PA21 template-template arguments key by template identity (or
	// pattern slot).
	if (arg.template_entity)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "t%p",
		         (const void*)arg.template_entity);
		out += buffer;
		return;
	}
	if (arg.template_param >= 0)
	{
		out += "tp" + to_string(arg.template_param);
		return;
	}
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
	// PA22 entity-valued arguments key by entity identity.
	if (arg.entity_scope)
	{
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "en%p:",
		         (const void*)arg.entity_scope);
		out += buffer;
		out += arg.entity_name;
		return;
	}
	out += "v";
	AppendKey(arg.type, out);
	out += ":" + to_string(arg.value_bits);
}

bool TemplateTemplateParamsMatch(const vector<TemplateParam>& formal,
                                 const vector<TemplateParam>& actual)
{
	size_t a = 0;
	for (size_t f = 0; f < formal.size(); f++)
	{
		if (formal[f].pack)
		{
			// The formal pack absorbs the remaining actual parameters
			// (kind-checked).
			for (; a < actual.size(); a++)
				if (actual[a].kind != formal[f].kind)
					return false;
			return f + 1 == formal.size();
		}
		if (a >= actual.size())
			return false;
		if (actual[a].kind != formal[f].kind || actual[a].pack)
			return false;
		a++;
	}
	return a == actual.size();
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
// spell true/false, enumerations the cast form "(Policy)2", signed
// types the signed decimal, everything else the unsigned decimal.
// Distinct values of one parameter cannot collide.
static string ValueSpelling(const TemplateArg& arg)
{
	// PA22 entity-valued arguments spell the entity's name.
	if (arg.entity_scope)
		return arg.entity_name;
	if (arg.type && arg.type->kind == TK_FUNDAMENTAL &&
	    arg.type->fundamental == FT_BOOL)
		return arg.value_bits ? "true" : "false";
	string digits = IsSignedIntegralFundamental(arg.value_type)
		? to_string((long long)arg.value_bits)
		: to_string(arg.value_bits);
	if (arg.type && arg.type->kind == TK_ENUM)
		return "(" + EntityScopePrefix(*arg.type->named) +
			arg.type->named->name + ")" + digits;
	return digits;
}

string TemplateArgumentSpelling(const vector<TemplateArg>& args)
{
	string text = "<";
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			text += ", ";
		const TemplateArg& arg = args[i];
		if (arg.template_entity)
			text += arg.template_entity->name;
		else if (arg.template_param >= 0)
			text += "#t" + to_string(arg.template_param);
		else if (!arg.is_value)
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

bool SameTemplateParameterKinds(const vector<TemplateParam>& a,
                                const vector<TemplateParam>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i].kind != b[i].kind || a[i].pack != b[i].pack)
			return false;
		if (a[i].kind == TPK_TEMPLATE &&
		    !SameTemplateParameterKinds(a[i].tt_params, b[i].tt_params))
			return false;
	}
	return true;
}

bool SameTemplateParameterLists(const vector<TemplateParam>& a,
                                const vector<TemplateParam>& b)
{
	if (!SameTemplateParameterKinds(a, b))
		return false;
	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i].kind != TPK_VALUE)
			continue;
		// The declared type of a non-type parameter is part of the
		// template's identity (14.5.6.1p6); spellings compare with the
		// parameter names positionalized and the declarator-id
		// stripped, mirroring CanonicalDeclaratorParams.
		if (!a[i].source || !b[i].source)
		{
			if (bool(a[i].source) != bool(b[i].source))
				return false;
			continue;
		}
		string left = FlattenSpecifierSeq(a[i].source->specifiers);
		string right = FlattenSpecifierSeq(b[i].source->specifiers);
		if (a[i].source->declarator)
		{
			string shape = FlattenDeclarator(*a[i].source->declarator);
			ReplaceWholeIdentifier(shape, a[i].name, "");
			left += "|" + shape;
		}
		if (b[i].source->declarator)
		{
			string shape = FlattenDeclarator(*b[i].source->declarator);
			ReplaceWholeIdentifier(shape, b[i].name, "");
			right += "|" + shape;
		}
		if (PositionalizeTemplateNames(left, a) !=
		    PositionalizeTemplateNames(right, b))
			return false;
	}
	return true;
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
