#include "sema/template_info.h"

#include <cstdio>

using std::to_string;

namespace {

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

string TemplateArgumentKey(const vector<TypePtr>& args)
{
	string key;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			key += ";";
		AppendKey(args[i], key);
	}
	return key;
}

string TemplateArgumentSpelling(const vector<TypePtr>& args)
{
	string text = "<";
	for (size_t i = 0; i < args.size(); i++)
	{
		if (i > 0)
			text += ", ";
		text += ArgSpelling(args[i]);
	}
	text += ">";
	return text;
}
