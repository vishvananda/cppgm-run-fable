#include "lowering/lower_name.h"

#include <cstdio>
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

// The named components (namespaces and classes) from the global scope
// down to `scope`, outermost first.
vector<string> ScopeComponents(const Scope* scope)
{
	vector<string> parts;
	for (; scope && scope->parent; scope = scope->parent)
		if ((scope->kind == SCOPE_NAMESPACE ||
		     scope->kind == SCOPE_CLASS) && !scope->name.empty())
			parts.insert(parts.begin(), scope->name);
	return parts;
}

// The named enclosing components (namespaces and classes) of a
// named-type entity, outermost first; unnamed components are skipped
// like the PA12 display qualification.
vector<string> EntityComponents(const NamedTypeInfo& info)
{
	vector<string> parts;
	for (const Scope* scope = info.scope; scope && scope->parent;
	     scope = scope->parent)
		if ((scope->kind == SCOPE_NAMESPACE ||
		     scope->kind == SCOPE_CLASS) && !scope->name.empty())
			parts.insert(parts.begin(), scope->name);
	parts.push_back(info.name);
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
	case TK_ARRAY:
	{
		string spelling = "A" +
			(type->bound_known ? to_string(type->bound) : string()) +
			"_" + MangleType(type->target, subs);
		return MangleSubstitutable(spelling, spelling, subs);
	}
	case TK_FUNCTION:
	{
		// Sequenced explicitly: the return type registers its
		// substitutions before the parameters.
		string return_part = MangleType(type->target, subs);
		string param_part = MangleBareParameters(type, subs);
		string spelling = "F" + return_part + param_part + "E";
		return MangleSubstitutable(spelling, spelling, subs);
	}
	case TK_CLASS:
	case TK_ENUM:
	{
		// Substitution keys stay name-based ("T:n::E") so the same
		// entity declared in two translation units compresses alike;
		// a substituted prefix compresses the nested spelling (NS_...).
		vector<string> parts = EntityComponents(*type->named);
		if (parts.size() == 1)
			return MangleSubstitutable("T:" + parts[0],
			                           SourceName(parts[0]), subs);
		vector<string> keys(parts.size());
		for (size_t i = 0; i < parts.size(); i++)
			keys[i] = (i ? keys[i - 1] + "::" : string("T:")) + parts[i];
		string found = subs.Find(keys.back());
		if (!found.empty())
			return found;
		size_t start = 0;
		string head;
		for (size_t k = parts.size() - 1; k > 0; k--)
		{
			string sub = subs.Find(keys[k - 1]);
			if (!sub.empty())
			{
				head = sub;
				start = k;
				break;
			}
		}
		string spelling = "N" + head;
		for (size_t i = start; i < parts.size(); i++)
		{
			spelling += SourceName(parts[i]);
			if (i + 1 < parts.size())
				subs.Add(keys[i]);
		}
		spelling += "E";
		return MangleSubstitutable(keys.back(), spelling, subs);
	}
	default:
		throw OutsideBoundary("mangled type form");
	}
}

string MangleTerminalName(const string& name, size_t arity)
{
	if (name.compare(0, 9, "operator ") == 0)
	{
		string text = name.substr(9);
		// 5.1.2: the unary forms of the dual-meaning operators carry
		// their own codes.
		if (arity == 1)
		{
			if (text == "+") return "ps";
			if (text == "-") return "ng";
			if (text == "&") return "ad";
			if (text == "*") return "de";
		}
		return OperatorCode(text);
	}
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

string LowerScopeKey(const Scope* scope)
{
	string key;
	for (; scope && scope->parent; scope = scope->parent)
	{
		if (scope->kind != SCOPE_NAMESPACE && scope->kind != SCOPE_CLASS)
			continue;
		string part = scope->name;
		if (part.empty())
		{
			char tagged[32];
			snprintf(tagged, sizeof(tagged), "<anon:%p>",
			         (const void*)scope);
			part = tagged;
		}
		key = part + "::" + key;
	}
	return key;
}

string LowerSanitizeName(const string& name)
{
	// Spaces drop ("operator new" -> "operatornew"); other non-word
	// characters become underscores ("operator==" -> "operator__",
	// "~C" -> "_C").
	string out;
	for (size_t i = 0; i < name.size(); i++)
	{
		char c = name[i];
		bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_';
		if (word)
			out += c;
		else if (c != ' ')
			out += '_';
	}
	return out;
}

size_t LowerMemberOverloadIndex(const Scope* scope, const string& name,
                                const TypePtr& adjusted)
{
	const ScopeBinding* binding = FindOwnBinding(*scope, name);
	if (!binding || binding->kind != SB_FUNCTION ||
	    adjusted->parameters.empty())
		return 0;
	TypePtr object = adjusted->parameters[0]->target;
	bool is_const = false;
	bool is_volatile = false;
	TopCv(object, is_const, is_volatile);
	vector<TypePtr> declared_params(adjusted->parameters.begin() + 1,
	                                adjusted->parameters.end());
	TypePtr declared = MakeFunctionType(adjusted->target, declared_params,
	                                    adjusted->variadic);
	for (size_t i = 0; i <= binding->overloads.size(); i++)
	{
		const TypePtr& candidate =
			i == 0 ? binding->type : binding->overloads[i - 1];
		if (candidate->is_const == is_const &&
		    candidate->is_volatile == is_volatile &&
		    candidate->parameters.size() == declared_params.size() &&
		    TypeEquals(RemoveTopCv(candidate->target),
		               RemoveTopCv(declared->target)))
		{
			bool same = true;
			for (size_t j = 0; j < declared_params.size(); j++)
				if (!TypeEquals(candidate->parameters[j],
				                declared_params[j]))
					same = false;
			if (same)
				return i;
		}
	}
	return 0;
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
		encoding = MangleTerminalName(name, type->parameters.size());
	else
	{
		encoding = "N";
		string entity_key = "T:";
		for (size_t i = 0; i < parts.size(); i++)
		{
			entity_key += (i ? "::" : "") + parts[i];
			encoding += SourceName(parts[i]);
			subs.Add(entity_key);
		}
		encoding += MangleTerminalName(name, type->parameters.size()) +
			"E";
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

string MangleMemberFunctionObjectName(const Scope* scope,
                                      const string& name,
                                      const TypePtr& type,
                                      const string& special_code)
{
	Substitutions subs;
	vector<string> parts = ScopeComponents(scope);
	// The implicit object parameter carries the method cv-qualifiers
	// and is dropped from the bare signature.
	bool is_const = false;
	bool is_volatile = false;
	TypePtr bare = type;
	if (!type->parameters.empty())
	{
		TopCv(type->parameters[0]->target, is_const, is_volatile);
		vector<TypePtr> params(type->parameters.begin() + 1,
		                       type->parameters.end());
		bare = MakeFunctionType(type->target, params, type->variadic);
	}
	string encoding = "N";
	if (is_volatile)
		encoding += "V";
	if (is_const)
		encoding += "K";
	string entity_key = "T:";
	for (size_t i = 0; i < parts.size(); i++)
	{
		entity_key += (i ? "::" : "") + parts[i];
		encoding += SourceName(parts[i]);
		subs.Add(entity_key);
	}
	if (!special_code.empty())
		encoding += special_code;
	else
		// Member operators count the implicit object argument.
		encoding += MangleTerminalName(name,
		                               bare->parameters.size() + 1);
	encoding += "E";
	return "_Z" + encoding + MangleBareParameters(bare, subs);
}
