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

// One component of a qualified name. A class-template specialization
// component carries the template name plus its argument list and
// mangles as `NameI<args>E` (5.1.8), with the template name and the
// template-id both substitution candidates.
struct NameComponent
{
	NameComponent() : args(0) {}

	string name;
	const vector<TemplateArg>* args;
};

NameComponent ScopeComponent(const Scope* scope)
{
	NameComponent part;
	if (scope->entity && scope->entity->spec_template &&
	    !scope->entity->is_template_anchor)
	{
		part.name = scope->entity->spec_template->name;
		part.args = &scope->entity->spec_args;
	}
	else
		part.name = scope->name;
	return part;
}

// The named components (namespaces and classes) from the global scope
// down to `scope`, outermost first.
vector<NameComponent> ScopeComponents(const Scope* scope)
{
	vector<NameComponent> parts;
	for (; scope && scope->parent; scope = scope->parent)
		if ((scope->kind == SCOPE_NAMESPACE ||
		     scope->kind == SCOPE_CLASS) && !scope->name.empty())
			parts.insert(parts.begin(), ScopeComponent(scope));
	return parts;
}

// The named enclosing components (namespaces and classes) of a
// named-type entity, outermost first; unnamed components are skipped
// like the PA12 display qualification.
vector<NameComponent> EntityComponents(const NamedTypeInfo& info)
{
	vector<NameComponent> parts;
	for (const Scope* scope = info.scope; scope && scope->parent;
	     scope = scope->parent)
		if ((scope->kind == SCOPE_NAMESPACE ||
		     scope->kind == SCOPE_CLASS) && !scope->name.empty())
			parts.insert(parts.begin(), ScopeComponent(scope));
	NameComponent leaf;
	if (info.spec_template && !info.is_template_anchor)
	{
		leaf.name = info.spec_template->name;
		leaf.args = &info.spec_args;
	}
	else
		leaf.name = info.name;
	parts.push_back(leaf);
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
bool LookupOperatorCode(const string& text, string& code);

string OperatorCode(const string& text)
{
	string code;
	if (LookupOperatorCode(text, code))
		return code;
	throw OutsideBoundary("operator-function name");
}

bool LookupOperatorCode(const string& text, string& code)
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
		{
			code = table[i].code;
			return true;
		}
	return false;
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

string MangleType(const TypePtr& type, Substitutions& subs,
                  string* key_out = 0);

// Appends the parameter encodings of a function type ("v" when empty,
// trailing "z" when variadic).
string MangleBareParameters(const TypePtr& fn, Substitutions& subs,
                            string* key_out = 0)
{
	if (fn->parameters.empty() && !fn->variadic)
	{
		if (key_out)
			*key_out += "v";
		return "v";
	}
	string out;
	for (size_t i = 0; i < fn->parameters.size(); i++)
	{
		string key;
		out += MangleType(fn->parameters[i], subs, &key);
		if (key_out)
			*key_out += key + ",";
	}
	if (fn->variadic)
	{
		out += "z";
		if (key_out)
			*key_out += "z";
	}
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

// The structural key of a type, independent of any substitution state.
string TypeKey(const TypePtr& type)
{
	Substitutions throwaway;
	string key;
	MangleType(type, throwaway, &key);
	return key;
}

// The structural keys of one component under prefix key `prev`: the
// name key (a template's own name is a substitution candidate) and
// the full key (with the argument keys for a specialization).
// The structural key of one template argument (types recurse through
// TypeKey; values key by declared type + bits; pattern slots by their
// parameter index).
string ArgKey(const TemplateArg& arg)
{
	if (arg.template_entity)
		return "tt:" + LowerScopeKey(arg.template_entity->declaring) +
			arg.template_entity->name;
	if (arg.template_param >= 0)
		return "ttp" + to_string(arg.template_param);
	if (!arg.is_value)
		return TypeKey(arg.type);
	if (arg.value_param >= 0)
		return "vp" + to_string(arg.value_param);
	if (arg.dependent_value)
		return "vdep";
	return "v" + TypeKey(arg.type) + ":" + to_string(arg.value_bits);
}

void ComponentKeys(const NameComponent& part, const string& prev,
                   string& name_key, string& full_key)
{
	name_key = (prev.empty() ? string("T:") : prev + "::") + part.name;
	full_key = name_key;
	if (part.args)
	{
		full_key += "<";
		for (size_t i = 0; i < part.args->size(); i++)
			full_key += ArgKey((*part.args)[i]) + ",";
		full_key += ">";
	}
}

string MangleComponentList(const vector<NameComponent>& parts,
                           Substitutions& subs, string* key_out);

// 5.1.6/5.1.7: one template argument. A concrete value spells the
// literal form `L <type> <value> E` (negative values with `n`); a
// pattern reference to the n-th template parameter spells T_/Tn_; a
// dependent expression spells a fixed placeholder (`object=` names
// are pairing hints only - the placeholder keeps mangling total and
// repeatable without encoding the full expression grammar).
string MangleTemplateArg(const TemplateArg& arg, Substitutions& subs)
{
	if (arg.template_entity)
	{
		// 5.1.6: a template-template argument spells the template's
		// (possibly nested) name like a class name.
		vector<NameComponent> parts =
			ScopeComponents(arg.template_entity->declaring);
		NameComponent leaf;
		leaf.name = arg.template_entity->name;
		parts.push_back(leaf);
		return MangleComponentList(parts, subs, 0);
	}
	if (arg.template_param >= 0)
		return arg.template_param == 0
			? string("T_")
			: "T" + to_string(arg.template_param - 1) + "_";
	if (!arg.is_value)
		return MangleType(arg.type, subs);
	if (arg.value_param >= 0)
		return arg.value_param == 0
			? string("T_") : "T" + to_string(arg.value_param - 1) + "_";
	if (arg.dependent_value)
		return "L_DEPE";
	string code = MangleType(arg.type, subs);
	string digits;
	if (IsSignedIntegralFundamental(arg.value_type) &&
	    (long long)arg.value_bits < 0)
		// Unsigned negation: the magnitude of the most negative value
		// stays representable.
		digits = "n" + to_string(0ull - arg.value_bits);
	else
		digits = to_string(arg.value_bits);
	return "L" + code + digits + "E";
}

// Appends one component's spelling. A specialization registers its
// template name first (5.1.9: the template-name is a substitution
// candidate), then mangles its arguments in sequence; when the
// component opens the spelling, an already-registered template name
// compresses ("S_IiE").
void AppendComponentSpelling(const NameComponent& part,
                             const string& name_key, Substitutions& subs,
                             string& out, bool allow_name_substitution)
{
	if (!part.args)
	{
		out += SourceName(part.name);
		return;
	}
	string tname;
	if (allow_name_substitution)
		tname = subs.Find(name_key);
	if (tname.empty())
	{
		subs.Add(name_key);
		tname = SourceName(part.name);
	}
	out += tname + "I";
	for (size_t i = 0; i < part.args->size(); i++)
		out += MangleTemplateArg((*part.args)[i], subs);
	out += "E";
}

// The nested/unscoped encoding of a full component list (class and
// enum types, template-ids): compresses against the longest
// registered head and registers the candidates in appearance order.
string MangleComponentList(const vector<NameComponent>& parts,
                           Substitutions& subs, string* key_out)
{
	vector<string> name_keys(parts.size());
	vector<string> full_keys(parts.size());
	string prev;
	for (size_t i = 0; i < parts.size(); i++)
	{
		ComponentKeys(parts[i], prev, name_keys[i], full_keys[i]);
		prev = full_keys[i];
	}
	if (key_out)
		*key_out = full_keys.back();
	string found = subs.Find(full_keys.back());
	if (!found.empty())
		return found;
	if (parts.size() == 2 && !parts[0].args && parts[0].name == "std" &&
	    !parts[1].args)
	{
		// 5.1.4.2: the abbreviation St spells the std prefix.
		subs.Add(full_keys.back());
		return "St" + SourceName(parts[1].name);
	}
	// Longest substituted head: the leaf's own template name (it
	// covers the whole prefix), then the longest prefix component.
	size_t start = 0;
	string head;
	bool head_is_leaf_name = false;
	if (parts.back().args)
	{
		string sub = subs.Find(name_keys.back());
		if (!sub.empty())
		{
			head = sub;
			head_is_leaf_name = true;
			start = parts.size() - 1;
		}
	}
	if (head.empty())
		for (size_t k = parts.size() - 1; k > 0; k--)
		{
			string sub = subs.Find(full_keys[k - 1]);
			if (!sub.empty())
			{
				head = sub;
				start = k;
				break;
			}
		}
	string body = head;
	if (head_is_leaf_name)
	{
		const NameComponent& leaf = parts.back();
		body += "I";
		for (size_t i = 0; i < leaf.args->size(); i++)
			body += MangleTemplateArg((*leaf.args)[i], subs);
		body += "E";
	}
	else
		for (size_t i = start; i < parts.size(); i++)
		{
			AppendComponentSpelling(parts[i], name_keys[i], subs, body,
			                        body.empty());
			if (i + 1 < parts.size())
				subs.Add(full_keys[i]);
		}
	subs.Add(full_keys.back());
	if (parts.size() > 1)
		body = "N" + body + "E";
	return body;
}

// Appends the enclosing components of a symbol's nested-name prefix,
// registering each as a substitution candidate. Returns the prefix
// key for the terminal component.
string ManglePrefixComponents(const vector<NameComponent>& parts,
                              Substitutions& subs, string& encoding)
{
	string prev;
	for (size_t i = 0; i < parts.size(); i++)
	{
		string name_key;
		string full_key;
		ComponentKeys(parts[i], prev, name_key, full_key);
		AppendComponentSpelling(parts[i], name_key, subs, encoding,
		                        false);
		subs.Add(full_key);
		prev = full_key;
	}
	return prev;
}

// The substitution keys are structural (composed from the entity
// identities, not the possibly substituted spellings), so the same
// type always finds its earlier registration.
// The cv-qualified head of a type encoding ("K"/"V"/"VK" plus the
// unqualified encoding), itself a substitution candidate.
string MangleCvQualified(const TypePtr& type, Substitutions& subs,
                         string* key_out)
{
	Type bare = *type;
	bare.is_const = false;
	bare.is_volatile = false;
	TypePtr unqualified(new Type(bare));
	string inner_key;
	string inner = MangleType(unqualified, subs, &inner_key);
	string cv = string(type->is_volatile ? "V" : "") +
		(type->is_const ? "K" : "");
	string key = cv + "|" + inner_key;
	if (key_out)
		*key_out = key;
	string found = subs.Find(key);
	if (!found.empty())
		return found;
	subs.Add(key);
	return cv + inner;
}

// 5.1.7: the enclosing function scope of a local entity (null when
// the entity is not function-local).
const Scope* LocalEntityFunctionScope(const NamedTypeInfo& info)
{
	for (const Scope* scope = info.scope; scope && scope->parent;
	     scope = scope->parent)
	{
		if (scope->kind == SCOPE_FUNCTION)
			return scope;
		if (scope->kind == SCOPE_NAMESPACE)
			return 0;
	}
	return 0;
}

// 5.1.7: a function-local entity mangles as
// `Z <function encoding> E <entity name>`.
string MangleLocalName(const NamedTypeInfo& info, const Scope* fn_scope,
                       Substitutions& subs, string* key_out)
{
	string fn_object;
	const Scope* declaring = fn_scope->parent;
	// The body scope carries its own composed type; a name lookup
	// would find only the first overload of the name.
	TypePtr fn_type = fn_scope->fn_type;
	if (!fn_type && declaring)
		if (const ScopeBinding* fn_binding =
		        FindOwnBinding(*declaring, fn_scope->name))
			fn_type = fn_binding->type;
	if (!fn_type || !declaring)
		throw OutsideBoundary("local entity mangling context");
	if (declaring->kind == SCOPE_CLASS)
		fn_object = MangleMemberFunctionObjectName(
			declaring, fn_scope->name, fn_type, "");
	else
		fn_object = MangleFunctionObjectName(declaring, fn_scope->name,
		                                     fn_type);
	// The encoding is the object name without its _Z prefix.
	string fn_encoding = fn_object.compare(0, 2, "_Z") == 0
		? fn_object.substr(2) : fn_object;
	// The classes between the function and the entity, then the leaf.
	string names;
	string name_key;
	for (const Scope* scope = info.scope;
	     scope && scope != fn_scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS && !scope->name.empty())
		{
			names = SourceName(scope->name) + names;
			name_key = scope->name + "::" + name_key;
		}
	names += SourceName(info.name);
	name_key += info.name;
	string key = "Z:" + fn_encoding + ":" + name_key;
	if (key_out)
		*key_out = key;
	string found = subs.Find(key);
	if (!found.empty())
		return found;
	subs.Add(key);
	return "Z" + fn_encoding + "E" + names;
}

string MangleType(const TypePtr& type, Substitutions& subs,
                  string* key_out)
{
	if ((type->is_const || type->is_volatile) &&
	    type->kind != TK_FUNCTION)
		return MangleCvQualified(type, subs, key_out);
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		if (key_out)
			*key_out = BuiltinCode(type->fundamental);
		return BuiltinCode(type->fundamental);
	case TK_POINTER:
	{
		string inner_key;
		string inner = MangleType(type->target, subs, &inner_key);
		string key = "P|" + inner_key;
		if (key_out)
			*key_out = key;
		string found = subs.Find(key);
		if (!found.empty())
			return found;
		subs.Add(key);
		return "P" + inner;
	}
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	{
		const char* mark = type->kind == TK_LVALUE_REFERENCE ? "R" : "O";
		string inner_key;
		string inner = MangleType(type->target, subs, &inner_key);
		string key = string(mark) + "|" + inner_key;
		if (key_out)
			*key_out = key;
		string found = subs.Find(key);
		if (!found.empty())
			return found;
		subs.Add(key);
		return mark + inner;
	}
	case TK_ARRAY:
	{
		string bound = type->bound_known ? to_string(type->bound)
		                                 : string();
		string inner_key;
		string inner = MangleType(type->target, subs, &inner_key);
		string key = "A" + bound + "|" + inner_key;
		if (key_out)
			*key_out = key;
		string found = subs.Find(key);
		if (!found.empty())
			return found;
		subs.Add(key);
		return "A" + bound + "_" + inner;
	}
	case TK_FUNCTION:
	{
		// Sequenced explicitly: the return type registers its
		// substitutions before the parameters.
		string return_key;
		string return_part = MangleType(type->target, subs, &return_key);
		string param_key;
		string param_part = MangleBareParameters(type, subs, &param_key);
		string key = "F|" + return_key + "|" + param_key;
		if (key_out)
			*key_out = key;
		string found = subs.Find(key);
		if (!found.empty())
			return found;
		subs.Add(key);
		return "F" + return_part + param_part + "E";
	}
	case TK_CLASS:
	case TK_ENUM:
		// 5.1.7: function-local entities take the Z...E local form.
		if (const Scope* fn_scope = LocalEntityFunctionScope(*type->named))
			return MangleLocalName(*type->named, fn_scope, subs,
			                       key_out);
		// Substitution keys stay name-based ("T:n::E") so the same
		// entity declared in two translation units compresses alike;
		// a substituted prefix compresses the nested spelling (NS_...).
		return MangleComponentList(EntityComponents(*type->named), subs,
		                           key_out);
	case TK_TEMPLATE_SPEC:
	{
		// A dependent specialization pattern (`box<T>`): the anchor's
		// components with this node's argument patterns on the leaf.
		vector<NameComponent> parts = EntityComponents(*type->named);
		parts.back().args = &type->targs;
		return MangleComponentList(parts, subs, key_out);
	}
	case TK_TYPE_PARAM:
	{
		// 5.1.5: the n-th template parameter spells T_/T<n-1>_ and is
		// a substitution candidate.
		int index = type->named->param_index;
		if (index < 0)
			throw OutsideBoundary("mangled type form");
		string key = "TP:" + to_string(index);
		if (key_out)
			*key_out = key;
		string spelling = index == 0
			? string("T_") : "T" + to_string(index - 1) + "_";
		return MangleSubstitutable(key, spelling, subs);
	}
	default:
		throw OutsideBoundary("mangled type form");
	}
}

string MangleTerminalName(const string& name, size_t arity)
{
	if (name.compare(0, 10, "operator \"") == 0)
		// Literal operators: li <source-name of the suffix>.
		return "li" + SourceName(name.substr(11));
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
	vector<string> parts;
	for (; scope && scope->parent; scope = scope->parent)
	{
		if (scope->kind != SCOPE_NAMESPACE && scope->kind != SCOPE_CLASS)
			continue;
		if (scope->name.empty())
		{
			// 7.3.1.1: the unnamed namespace spells its ABI-style
			// placeholder component.
			if (scope->kind == SCOPE_NAMESPACE)
				parts.insert(parts.begin(), "_GLOBAL__N_1");
			continue;
		}
		parts.insert(parts.begin(), scope->name);
	}
	string path;
	// PA18: specialization scopes spell their argument list
	// ("Box<int>"); the symbol characters sanitize per component.
	for (size_t i = 0; i < parts.size(); i++)
		path += LowerSanitizeName(parts[i]) + "__";
	return path;
}

// Whether a namespace/class scope sits inside a function body (any
// non-namespace/class/template-params container on the way up).
static bool ScopeIsFunctionLocal(const Scope* scope)
{
	for (const Scope* up = scope->parent; up; up = up->parent)
	{
		if (up->kind == SCOPE_NAMESPACE)
			return false;
		if (up->kind != SCOPE_CLASS &&
		    up->kind != SCOPE_TEMPLATE_PARAMS)
			return true;
	}
	return false;
}

// Whether a template-argument type names (recursively) only named
// namespace/class-scoped entities, so its spelling is unique
// program-wide. Local classes in different functions can share a
// spelling (`Maker::Piece`), so specializations over them must not
// key by name.
static bool ArgSpellingIsGlobal(const TemplateArg& arg);

static bool ArgSpellingIsGlobal(const TypePtr& type)
{
	if (!type)
		return false;
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return true;
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	case TK_ARRAY:
		return ArgSpellingIsGlobal(type->target);
	case TK_CLASS:
	case TK_ENUM:
	{
		const NamedTypeInfo* named = type->named;
		if (!named || named->name.empty())
			return false;
		for (const Scope* up = named->scope; up && up->parent;
		     up = up->parent)
		{
			if (up->kind == SCOPE_TEMPLATE_PARAMS)
				continue;
			if (up->kind != SCOPE_NAMESPACE && up->kind != SCOPE_CLASS)
				return false;
			if (up->name.empty())
				return false;  // unnamed namespace: per-unit identity
		}
		for (size_t i = 0; i < named->spec_args.size(); i++)
			if (!ArgSpellingIsGlobal(named->spec_args[i]))
				return false;
		return true;
	}
	default:
		return false;
	}
}

// A concrete value argument spells its canonical decimal (globally
// unique per parameter); an enum-typed value additionally requires the
// enum's own name to be global.
static bool ArgSpellingIsGlobal(const TemplateArg& arg)
{
	if (arg.template_entity)
	{
		for (const Scope* up = arg.template_entity->declaring;
		     up && up->parent; up = up->parent)
		{
			if (up->kind == SCOPE_TEMPLATE_PARAMS)
				continue;
			if (up->kind != SCOPE_NAMESPACE && up->kind != SCOPE_CLASS)
				return false;
			if (up->name.empty())
				return false;
		}
		return true;
	}
	if (arg.template_param >= 0)
		return false;
	if (!arg.is_value)
		return ArgSpellingIsGlobal(arg.type);
	if (arg.value_param >= 0 || arg.dependent_value)
		return false;
	if (arg.type && (arg.type->kind == TK_CLASS ||
	                 arg.type->kind == TK_ENUM))
		return ArgSpellingIsGlobal(arg.type);
	return true;
}

// Whether a scope component's name is a program-wide identity: named,
// not function-local, and (for specializations) spelled over globally
// unique argument names.
static bool ScopeNameIsGlobal(const Scope* scope)
{
	if (scope->name.empty() || ScopeIsFunctionLocal(scope))
		return false;
	if (scope->entity)
		for (size_t i = 0; i < scope->entity->spec_args.size(); i++)
			if (!ArgSpellingIsGlobal(scope->entity->spec_args[i]))
				return false;
	return true;
}

string LowerScopeKey(const Scope* scope)
{
	string key;
	for (; scope && scope->parent; scope = scope->parent)
	{
		if (scope->kind != SCOPE_NAMESPACE && scope->kind != SCOPE_CLASS)
			continue;
		// Globally named scopes key by name, so the same entity
		// declared in several translation units merges onto one entry
		// (specialization scope names carry their qualified argument
		// spellings). Unnamed scopes, function-local classes, and
		// specializations over local names carry the scope object
		// instead: same-named locals in different functions must stay
		// distinct, and unnamed namespaces are per-unit by definition.
		// Keys are internal; names render elsewhere.
		if (ScopeNameIsGlobal(scope))
		{
			key = scope->name + "::" + key;
			continue;
		}
		char tagged[48];
		snprintf(tagged, sizeof(tagged), "%s#%p", scope->name.c_str(),
		         (const void*)scope);
		key = string(tagged) + "::" + key;
	}
	return key;
}

string LowerSanitizeName(const string& name)
{
	// Every non-word character (including spaces) becomes an
	// underscore ("operator==" -> "operator__", "~C" -> "_C",
	// "Box<int, 7>" -> "Box_int__7_"): the checked references pin the
	// space-as-underscore form for specialization scope names, and
	// function-symbol spellings canonicalize in comparison anyway.
	string out;
	for (size_t i = 0; i < name.size(); i++)
	{
		char c = name[i];
		bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_';
		out += word ? c : '_';
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
		    candidate->ref_qual == adjusted->ref_qual &&
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
	vector<NameComponent> parts = ScopeComponents(scope);
	string encoding;
	if (parts.empty())
		encoding = MangleTerminalName(name, type->parameters.size());
	else
	{
		string prefix;
		ManglePrefixComponents(parts, subs, prefix);
		encoding = "N" + prefix +
			MangleTerminalName(name, type->parameters.size()) + "E";
	}
	return "_Z" + encoding + MangleBareParameters(type, subs);
}

string MangleFunctionTemplateObjectName(const FunctionSpecialization& spec)
{
	// 5.1.2/5.1.8: <name> I <args> E with the return type included;
	// the signature mangles from the abstract pattern so parameter
	// references spell T_/Tn_ (falling back to the concrete signature
	// when the pattern has non-composing dependent forms).
	const TemplateInfo& tmpl = *spec.owner;
	Substitutions subs;
	vector<NameComponent> parts = ScopeComponents(tmpl.declaring);
	// PA21 member function templates: the encoding is the member form
	// (method cv/ref-qualifiers before the class prefix).
	bool member = tmpl.declaring && tmpl.declaring->kind == SCOPE_CLASS;
	TypePtr pattern = tmpl.pattern ? tmpl.pattern : spec.type;
	// A pattern with an unexpanded pack parameter mangles from the
	// concrete signature (the pinned PA19 spellings).
	for (size_t i = 0; pattern.get() != spec.type.get() &&
	     i < pattern->parameters.size(); i++)
		if (pattern->parameters[i]->pack_expansion)
			pattern = spec.type;
	string cv;
	if (member)
	{
		if (pattern->is_volatile)
			cv += "V";
		if (pattern->is_const)
			cv += "K";
		if (pattern->ref_qual == 1)
			cv += "R";
		else if (pattern->ref_qual == 2)
			cv += "O";
	}
	string prefix;
	string prev = ManglePrefixComponents(parts, subs, prefix);
	string name_key = (prev.empty() ? string("T:") : prev + "::") +
		tmpl.name;
	subs.Add(name_key);
	string terminal =
		MangleTerminalName(tmpl.name, pattern->parameters.size() +
		                              (member ? 1 : 0));
	string targs;
	for (size_t i = 0; i < spec.args.size(); i++)
		targs += MangleTemplateArg(spec.args[i], subs);
	string result = MangleType(pattern->target, subs);
	string params = MangleBareParameters(pattern, subs);
	string encoding = terminal + "I" + targs + "E";
	if (!parts.empty() || member)
		encoding = "N" + cv + prefix + encoding + "E";
	return "_Z" + encoding + result + params;
}

// PA21 constructor-template specializations: C1/C2 followed by the
// template-argument list, with the parameters mangled from the
// pattern (`_ZN4pairIiEC1IiiLi0EEEOT_OT0_`).
string MangleMemberFunctionTemplateObjectName(
	const Scope* scope, const FunctionSpecialization& spec,
	const string& special_code)
{
	const TemplateInfo& tmpl = *spec.owner;
	Substitutions subs;
	vector<NameComponent> parts = ScopeComponents(scope);
	string encoding = "N";
	ManglePrefixComponents(parts, subs, encoding);
	TypePtr pattern = tmpl.pattern ? tmpl.pattern : spec.type;
	string targs;
	for (size_t i = 0; i < spec.args.size(); i++)
		targs += MangleTemplateArg(spec.args[i], subs);
	encoding += special_code + "I" + targs + "E";
	encoding += "E";
	return "_Z" + encoding + MangleBareParameters(pattern, subs);
}

string MangleVariableObjectName(const Scope* scope, const string& name)
{
	Substitutions subs;
	vector<NameComponent> parts = ScopeComponents(scope);
	if (parts.empty())
		return "_Z" + SourceName(name);
	if (parts.size() == 1 && !parts[0].args && parts[0].name == "std")
		// 5.1.4.2: the abbreviation St spells the std prefix.
		return "_ZSt" + SourceName(name);
	string prefix;
	ManglePrefixComponents(parts, subs, prefix);
	return "_ZN" + prefix + SourceName(name) + "E";
}

string MangleClassTypeEncoding(const NamedTypeInfo* entity)
{
	Substitutions subs;
	return MangleType(MakeNamedType(TK_CLASS, entity), subs);
}

string MangleMemberFunctionObjectName(const Scope* scope,
                                      const string& name,
                                      const TypePtr& type,
                                      const string& special_code)
{
	Substitutions subs;
	vector<NameComponent> parts = ScopeComponents(scope);
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
	if (type->ref_qual == 1)
		encoding += "R";
	else if (type->ref_qual == 2)
		encoding += "O";
	ManglePrefixComponents(parts, subs, encoding);
	if (!special_code.empty())
		encoding += special_code;
	else
	{
		// A conversion function encodes "cv <type>"; member operators
		// count the implicit object argument.
		string code;
		bool conversion = name.compare(0, 9, "operator ") == 0 &&
			name.compare(0, 10, "operator \"") != 0 &&
			!LookupOperatorCode(name.substr(9), code);
		if (conversion)
			encoding += "cv" + MangleType(bare->target, subs);
		else
			encoding += MangleTerminalName(
				name, bare->parameters.size() + 1);
	}
	encoding += "E";
	return "_Z" + encoding + MangleBareParameters(bare, subs);
}
