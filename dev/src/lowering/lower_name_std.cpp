// 5.1.4.2 standard-namespace abbreviations of the Itanium object-name
// mangler: the Ss/Si/So/Sd whole-specialization codes and the Sa/Sb
// template-name codes, applied to complete type spellings
// (MangleComponentList) and to nested-name heads
// (ManglePrefixComponents). Direct standard substitutions never enter
// the numbered substitution table; SaI...E template-ids still
// register their chain keys.

#include "lowering/lower_name.h"

#include "lowering/lower_name_parts.h"

using std::vector;

namespace lower_mangle {

// --- ::std abbreviations (5.1.4.2 / 5.1.5) ---------------------------------

// A leading "std" component from ScopeComponents/EntityComponents is
// the global ::std namespace (the walk starts at the global scope).
bool HeadIsStd(const vector<NameComponent>& parts)
{
	return !parts.empty() && !parts[0].args && parts[0].name == "std";
}

bool ArgIsChar(const TemplateArg& arg)
{
	return !arg.is_value && !arg.dependent_type && !arg.template_entity &&
		arg.template_param < 0 && arg.type &&
		arg.type->kind == TK_FUNDAMENTAL && !arg.type->is_const &&
		!arg.type->is_volatile && arg.type->fundamental == FT_CHAR;
}

// True for the class-type argument `tmpl_name<char>` where tmpl_name
// is a class template declared directly inside ::std.
bool ArgIsStdCharSpec(const TemplateArg& arg, const char* tmpl_name)
{
	if (arg.is_value || arg.dependent_type || arg.template_entity ||
	    arg.template_param >= 0 || !arg.type ||
	    arg.type->kind != TK_CLASS || arg.type->is_const ||
	    arg.type->is_volatile)
		return false;
	const NamedTypeInfo* named = arg.type->named;
	if (!named || !named->spec_template ||
	    named->spec_template->name != tmpl_name ||
	    named->spec_args.size() != 1 || !ArgIsChar(named->spec_args[0]))
		return false;
	const Scope* declaring = named->spec_template->declaring;
	return declaring && declaring->kind == SCOPE_NAMESPACE &&
		declaring->name == "std" && declaring->parent &&
		!declaring->parent->parent;
}

// The abbreviation of a whole specialization directly inside ::std
// (Ss/Si/So/Sd), or "" when none applies. The abbreviated spelling is
// itself a substitution and registers no candidates.
const char* StdSpecializationAbbreviation(const NameComponent& part)
{
	if (!part.args)
		return "";
	const vector<TemplateArg>& args = *part.args;
	bool char_stream = args.size() == 2 && ArgIsChar(args[0]) &&
		ArgIsStdCharSpec(args[1], "char_traits");
	if (part.name == "basic_string")
		return args.size() == 3 && ArgIsChar(args[0]) &&
			ArgIsStdCharSpec(args[1], "char_traits") &&
			ArgIsStdCharSpec(args[2], "allocator") ? "Ss" : "";
	if (part.name == "basic_istream")
		return char_stream ? "Si" : "";
	if (part.name == "basic_ostream")
		return char_stream ? "So" : "";
	if (part.name == "basic_iostream")
		return char_stream ? "Sd" : "";
	return "";
}

// The abbreviation of a template name directly inside ::std (Sa/Sb),
// or "" when none applies. The name spells the code and never enters
// the substitution table; a template-id over it still registers.
const char* StdTemplateAbbreviation(const NameComponent& part)
{
	if (part.name == "allocator")
		return "Sa";
	if (part.name == "basic_string")
		return "Sb";
	return "";
}

// Spells one component that is a direct member of ::std, honoring the
// abbreviation catalog; registers the same candidates the fully
// spelled form would (minus the abbreviated names themselves).
// `register_full` reports whether the caller's full-key registration
// still applies.
string SpellStdMemberComponent(const NameComponent& part,
                               const string& name_key,
                               Substitutions& subs, bool& register_full)
{
	register_full = true;
	const char* spec_code = StdSpecializationAbbreviation(part);
	if (*spec_code)
	{
		register_full = false;
		return spec_code;
	}
	const char* tmpl_code = StdTemplateAbbreviation(part);
	if (*tmpl_code)
	{
		if (!part.args)
		{
			register_full = false;
			return tmpl_code;
		}
		string body = string(tmpl_code) + "I";
		body += MangleArgList(*part.args, part.pack_start,
		                      part.pack_end, subs);
		body += "E";
		return body;
	}
	if (part.args)
	{
		string name_sub = subs.Find(name_key);
		if (!name_sub.empty())
		{
			string body = name_sub + "I";
			body += MangleArgList(*part.args, part.pack_start,
			                      part.pack_end, subs);
			body += "E";
			return body;
		}
	}
	string body = "St";
	AppendComponentSpelling(part, name_key, subs, body, false);
	return body;
}

// Spells the leading [::std, parts[1]] pair of a nested name when a
// catalog abbreviation applies (5.1.4.2): a whole-spec abbreviation
// (Ss/Si/So/Sd) is a direct standard substitution and registers no
// numbered candidate; an Sa/Sb template-id spells `SaI...E` and its
// full chain key still registers. Returns the number of components
// consumed (0 when no abbreviation applies).
size_t SpellStdHeadAbbreviation(const vector<NameComponent>& parts,
                                const vector<string>& full_keys,
                                Substitutions& subs, string& out)
{
	if (parts.size() < 2)
		return 0;
	const NameComponent& part = parts[1];
	const char* spec_code = StdSpecializationAbbreviation(part);
	if (*spec_code)
	{
		out += spec_code;
		return 2;
	}
	const char* tmpl_code = StdTemplateAbbreviation(part);
	if (*tmpl_code && part.args)
	{
		out += tmpl_code;
		out += "I";
		out += MangleArgList(*part.args, part.pack_start, part.pack_end,
		                     subs);
		out += "E";
		subs.Add(full_keys[1]);
		return 2;
	}
	return 0;
}


}  // namespace lower_mangle
