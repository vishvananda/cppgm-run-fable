#include "lowering/lower_name.h"

#include <stdexcept>
#include <vector>

using std::runtime_error;
using std::to_string;
using std::vector;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

// The named-namespace components from the global scope down to
// `scope`, outermost first.
vector<string> ScopeComponents(const Scope* scope)
{
	vector<string> parts;
	for (; scope && scope->parent; scope = scope->parent)
		if (scope->kind == SCOPE_NAMESPACE && !scope->name.empty())
			parts.insert(parts.begin(), scope->name);
	return parts;
}

// --- Itanium mangling (the PA14 procedural subset) -------------------

const char* BuiltinCode(EFundamentalType type)
{
	switch (type)
	{
	case FT_VOID: return "v";
	case FT_BOOL: return "b";
	case FT_CHAR: return "c";
	case FT_SIGNED_CHAR: return "a";
	case FT_UNSIGNED_CHAR: return "h";
	case FT_SHORT_INT: return "s";
	case FT_UNSIGNED_SHORT_INT: return "t";
	case FT_INT: return "i";
	case FT_UNSIGNED_INT: return "j";
	case FT_LONG_INT: return "l";
	case FT_UNSIGNED_LONG_INT: return "m";
	case FT_LONG_LONG_INT: return "x";
	case FT_UNSIGNED_LONG_LONG_INT: return "y";
	case FT_WCHAR_T: return "w";
	case FT_CHAR16_T: return "Ds";
	case FT_CHAR32_T: return "Di";
	case FT_FLOAT: return "f";
	case FT_DOUBLE: return "d";
	case FT_LONG_DOUBLE: return "e";
	case FT_NULLPTR_T: return "Dn";
	}
	throw OutsideBoundary("mangled fundamental type");
}

// The 5.1.2 operator-function terminal codes.
string OperatorCode(const string& text)
{
	struct Entry { const char* op; const char* code; };
	static const Entry table[] = {
		{"new", "nw"}, {"new[]", "na"}, {"delete", "dl"},
		{"delete[]", "da"}, {"+", "pl"}, {"-", "mi"}, {"*", "ml"},
		{"/", "dv"}, {"%", "rm"}, {"&", "an"}, {"|", "or"}, {"^", "eo"},
		{"=", "aS"}, {"+=", "pL"}, {"-=", "mI"}, {"*=", "mL"},
		{"/=", "dV"}, {"%=", "rM"}, {"&=", "aN"}, {"|=", "oR"},
		{"^=", "eO"}, {"<<", "ls"}, {">>", "rs"}, {"<<=", "lS"},
		{">>=", "rS"}, {"==", "eq"}, {"!=", "ne"}, {"<", "lt"},
		{">", "gt"}, {"<=", "le"}, {">=", "ge"}, {"!", "nt"},
		{"~", "co"}, {"&&", "aa"}, {"||", "oo"}, {"++", "pp"},
		{"--", "mm"}, {",", "cm"}, {"->*", "pm"}, {"->", "pt"},
		{"()", "cl"}, {"[]", "ix"},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (text == table[i].op)
			return table[i].code;
	throw OutsideBoundary("operator-function name");
}

// Component substitution table (5.1.9): previously seen substitutable
// fragments compress to S_/S<n>_.
class Substitutions
{
public:
	// Returns the compressed spelling if `key` was seen, else "".
	string Find(const string& key) const
	{
		for (size_t i = 0; i < seen_.size(); i++)
		{
			if (seen_[i] != key)
				continue;
			if (i == 0)
				return "S_";
			return "S" + Base36(i - 1) + "_";
		}
		return "";
	}

	void Add(const string& key)
	{
		if (Find(key).empty())
			seen_.push_back(key);
	}

private:
	static string Base36(size_t value)
	{
		const char* digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
		string out;
		do
		{
			out.insert(out.begin(), digits[value % 36]);
			value /= 36;
		} while (value);
		return out;
	}

	vector<string> seen_;
};

string SourceName(const string& name)
{
	return to_string(name.size()) + name;
}

string MangleType(const TypePtr& type, Substitutions& subs);

// Appends the parameter encodings of a function type ("v" when empty,
// trailing "z" when variadic).
string MangleBareParameters(const TypePtr& fn, Substitutions& subs)
{
	if (fn->parameters.empty() && !fn->variadic)
		return "v";
	string out;
	for (size_t i = 0; i < fn->parameters.size(); i++)
		out += MangleType(fn->parameters[i], subs);
	if (fn->variadic)
		out += "z";
	return out;
}

string MangleSubstitutable(const string& key, const string& spelling,
                           Substitutions& subs)
{
	string found = subs.Find(key);
	if (!found.empty())
		return found;
	subs.Add(key);
	return spelling;
}

string MangleType(const TypePtr& type, Substitutions& subs)
{
	bool is_const = type->is_const;
	bool is_volatile = type->is_volatile;
	if (is_const || is_volatile)
	{
		Type bare = *type;
		bare.is_const = false;
		bare.is_volatile = false;
		TypePtr unqualified(new Type(bare));
		string inner = MangleType(unqualified, subs);
		string spelling = string(is_volatile ? "V" : "") +
			(is_const ? "K" : "") + inner;
		return MangleSubstitutable(spelling, spelling, subs);
	}
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return BuiltinCode(type->fundamental);
	case TK_POINTER:
	{
		string spelling = "P" + MangleType(type->target, subs);
		return MangleSubstitutable(spelling, spelling, subs);
	}
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	{
		string spelling =
			(type->kind == TK_LVALUE_REFERENCE ? "R" : "O") +
			MangleType(type->target, subs);
		return MangleSubstitutable(spelling, spelling, subs);
	}
	case TK_FUNCTION:
	{
		string spelling = "F" + MangleType(type->target, subs) +
			MangleBareParameters(type, subs) + "E";
		return MangleSubstitutable(spelling, spelling, subs);
	}
	case TK_CLASS:
	case TK_ENUM:
	{
		// The display spelling is "enum [class] [path::]Name"; recover
		// the path components after the keyword prefix.
		const string& display = type->named->display;
		size_t space = display.rfind(' ');
		string qualified = space == string::npos
			? display : display.substr(space + 1);
		vector<string> parts;
		size_t start = 0;
		for (size_t sep = qualified.find("::"); sep != string::npos;
		     sep = qualified.find("::", start))
		{
			parts.push_back(qualified.substr(start, sep - start));
			start = sep + 2;
		}
		parts.push_back(qualified.substr(start));
		if (parts.size() == 1)
			return MangleSubstitutable("T:" + qualified,
			                           SourceName(parts[0]), subs);
		string spelling = "N";
		string prefix_key = "T:";
		for (size_t i = 0; i + 1 < parts.size(); i++)
		{
			prefix_key += parts[i] + "::";
			spelling += SourceName(parts[i]);
			subs.Add(prefix_key);
		}
		spelling += SourceName(parts.back()) + "E";
		return MangleSubstitutable("T:" + qualified, spelling, subs);
	}
	default:
		throw OutsideBoundary("mangled type form");
	}
}

string MangleTerminalName(const string& name)
{
	if (name.compare(0, 9, "operator ") == 0)
		return OperatorCode(name.substr(9));
	return SourceName(name);
}

}  // namespace

string LowerScopePath(const Scope* scope)
{
	vector<string> parts = ScopeComponents(scope);
	string path;
	for (size_t i = 0; i < parts.size(); i++)
		path += parts[i] + "__";
	return path;
}

string LowerSanitizeName(const string& name)
{
	string out;
	for (size_t i = 0; i < name.size(); i++)
	{
		char c = name[i];
		bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_';
		if (word)
			out += c;
	}
	return out;
}

size_t LowerOverloadIndex(const Scope* scope, const string& name,
                          const TypePtr& type)
{
	const ScopeBinding* binding = FindOwnBinding(*scope, name);
	if (!binding || binding->kind != SB_FUNCTION)
		return 0;
	if (TypeEquals(binding->type, type))
		return 0;
	for (size_t i = 0; i < binding->overloads.size(); i++)
		if (TypeEquals(binding->overloads[i], type))
			return i + 1;
	return 0;
}

bool LowerOverloadDeleted(const Scope* scope, const string& name,
                          const TypePtr& type)
{
	const ScopeBinding* binding = FindOwnBinding(*scope, name);
	if (!binding || binding->kind != SB_FUNCTION)
		return false;
	size_t index = LowerOverloadIndex(scope, name, type);
	return index < binding->fn_deleted.size() &&
		binding->fn_deleted[index];
}

bool LowerInUnnamedNamespace(const Scope* scope)
{
	for (; scope && scope->parent; scope = scope->parent)
		if (scope->kind == SCOPE_NAMESPACE && scope->name.empty())
			return true;
	return false;
}

string MangleFunctionObjectName(const Scope* scope, const string& name,
                                const TypePtr& type)
{
	Substitutions subs;
	vector<string> parts = ScopeComponents(scope);
	string encoding;
	if (parts.empty())
		encoding = MangleTerminalName(name);
	else
	{
		encoding = "N";
		string prefix_key = "T:";
		for (size_t i = 0; i < parts.size(); i++)
		{
			prefix_key += parts[i] + "::";
			encoding += SourceName(parts[i]);
			subs.Add(prefix_key);
		}
		encoding += MangleTerminalName(name) + "E";
	}
	return "_Z" + encoding + MangleBareParameters(type, subs);
}

string MangleVariableObjectName(const Scope* scope, const string& name)
{
	vector<string> parts = ScopeComponents(scope);
	if (parts.empty())
		return "_Z" + SourceName(name);
	string encoding = "N";
	for (size_t i = 0; i < parts.size(); i++)
		encoding += SourceName(parts[i]);
	return "_Z" + encoding + SourceName(name) + "E";
}
