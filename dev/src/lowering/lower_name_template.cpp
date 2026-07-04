#include "lowering/lower_name.h"

#include <stdexcept>
#include <vector>

#include "ast/ast.h"
#include "ast/ast_expr.h"
#include "lowering/lower_name_parts.h"

using std::to_string;
using std::vector;

// PA22 function-template object names. The Itanium signature of a
// specialization spells the template's as-written pattern (5.1.5.2):
// composable pieces mangle from the typed pattern (T_/Tn_ leaves,
// `Dp<element>` packs), and dependent pieces that do not compose into
// types (`typename remove_reference<T>::type&`) mangle syntactically
// from their written AST - the ABI rule for dependent names. Written
// and typed components share substitution keys so they compress
// against each other exactly like the reference spellings.

namespace lower_mangle {

namespace {

// The declarator of a function-template pattern declaration.
const AstDeclarator* WrittenDeclarator(const AstDecl& inner)
{
	if (inner.kind == DK_SIMPLE)
		return inner.declarators.empty()
			? 0 : inner.declarators[0].declarator.get();
	return inner.declarator.get();
}

// The parameter clause of a function declarator (the first DI_PARAMS
// item, through one nesting level).
const AstParameterClause* WrittenParameterClause(
	const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return item.params.get();
		if (item.kind == DI_NESTED && item.nested)
			if (const AstParameterClause* inner =
			        WrittenParameterClause(*item.nested))
				return inner;
	}
	return 0;
}

// The position of the named template parameter of `kind`, -1 when the
// name is not a parameter reference.
int TemplateParamIndex(const Substitutions& subs, ETemplateParamKind kind,
                       const string& id)
{
	if (!subs.pattern_params)
		return -1;
	const vector<TemplateParam>& params = *subs.pattern_params;
	for (size_t p = 0; p < params.size(); p++)
		if (params[p].kind == kind && params[p].name == id)
			return (int)p;
	return -1;
}

string ParamSpelling(int index)
{
	return index == 0 ? string("T_") : "T" + to_string(index - 1) + "_";
}

// 5.1.5: the builtin code of a written simple-type keyword sequence
// (space-joined source order).
string FundamentalKeywordCode(const string& keywords)
{
	struct Entry { const char* text; const char* code; };
	static const Entry table[] = {
		{"void", "v"}, {"bool", "b"}, {"char", "c"},
		{"signed char", "a"}, {"unsigned char", "h"},
		{"short", "s"}, {"short int", "s"},
		{"unsigned short", "t"}, {"unsigned short int", "t"},
		{"int", "i"}, {"signed", "i"}, {"signed int", "i"},
		{"unsigned", "j"}, {"unsigned int", "j"},
		{"long", "l"}, {"long int", "l"},
		{"unsigned long", "m"}, {"unsigned long int", "m"},
		{"long long", "x"}, {"long long int", "x"},
		{"unsigned long long", "y"}, {"unsigned long long int", "y"},
		{"wchar_t", "w"}, {"char16_t", "Ds"}, {"char32_t", "Di"},
		{"float", "f"}, {"double", "d"}, {"long double", "e"},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (keywords == table[i].text)
			return table[i].code;
	throw OutsideBoundary("mangled dependent type form");
}

string MangleWrittenTypeId(const AstTypeId& id, Substitutions& subs,
                           string* key_out);

string MangleWrittenExpr(const AstExpr& expr, Substitutions& subs,
                         string* key_out)
{
	// The written expression subset inside a dependent template-id: a
	// value-parameter reference spells `X T_ E` (expressions are not
	// substitution candidates).
	if (expr.kind == EK_ID && expr.name.parts.size() == 1 &&
	    expr.name.parts[0].kind == NP_IDENTIFIER &&
	    !expr.name.parts[0].tilde)
	{
		int index = TemplateParamIndex(subs, TPK_VALUE,
		                               expr.name.parts[0].identifier);
		if (index >= 0)
		{
			if (key_out)
				*key_out = "vp" + to_string(index);
			return "X" + ParamSpelling(index) + "E";
		}
	}
	throw OutsideBoundary("mangled dependent argument form");
}

string MangleWrittenArgElement(const AstTemplateArgument& argument,
                               Substitutions& subs, string* key_out)
{
	if (argument.is_type && argument.type)
		return MangleWrittenTypeId(*argument.type, subs, key_out);
	if (!argument.is_type && argument.expr)
		return MangleWrittenExpr(*argument.expr, subs, key_out);
	throw OutsideBoundary("mangled dependent argument form");
}

// One written template-argument; a `...` expansion spells an argument
// pack holding the expansion, `J Dp <element> E`, with the Dp
// component a substitution candidate. The declarator parser records a
// type-argument's `...` as a DI_PACK item (`tuple<T...>`), so the
// expansion mark is read from either place.
string MangleWrittenArg(const AstTemplateArgument& argument,
                        Substitutions& subs, string* key_out)
{
	bool pack = argument.pack;
	if (argument.is_type && argument.type && argument.type->declarator)
		for (size_t i = 0;
		     i < argument.type->declarator->items.size(); i++)
			if (argument.type->declarator->items[i].kind == DI_PACK)
				pack = true;
	if (!pack)
		return MangleWrittenArgElement(argument, subs, key_out);
	string key;
	string inner = MangleWrittenArgElement(argument, subs, &key);
	string spelled = MangleSubstitutable("Dp|" + key, "Dp" + inner, subs);
	if (key_out)
		*key_out = key + "...";
	return "J" + spelled + "E";
}

// The structural key of one written argument (aligned with the typed
// ArgKey format), independent of the live substitution state.
string WrittenArgKey(const AstTemplateArgument& argument,
                     const Substitutions& subs)
{
	Substitutions throwaway;
	throwaway.pattern_params = subs.pattern_params;
	string key;
	MangleWrittenArg(argument, throwaway, &key);
	return key;
}

// 5.1.8 dependent names: a written form (`typename C::value_type`,
// `tuple_element<I, tuple<T...> >::type`) mangles syntactically with
// template-parameter references spelled T_/Tn_ and each prefix a
// substitution candidate.
string MangleDependentName(const AstName& name, Substitutions& subs,
                           string* key_out)
{
	if (name.parts.empty())
		throw OutsideBoundary("mangled dependent name form");
	string body;
	string prev_key;
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if ((part.kind != NP_IDENTIFIER && part.kind != NP_TEMPLATE_ID) ||
		    part.tilde)
			throw OutsideBoundary("mangled dependent name form");
		if (i == 0 && part.kind == NP_IDENTIFIER)
		{
			// A leading template-parameter reference.
			int index = TemplateParamIndex(subs, TPK_TYPE,
			                               part.identifier);
			if (index >= 0)
			{
				prev_key = "TP:" + to_string(index);
				body = MangleSubstitutable(prev_key,
				                           ParamSpelling(index), subs);
				continue;
			}
		}
		string name_key = (prev_key.empty() ? string("T:")
		                                    : prev_key + "::") +
			part.identifier;
		string full_key = name_key;
		if (part.kind == NP_TEMPLATE_ID)
		{
			full_key += "<";
			for (size_t a = 0; a < part.arguments.size(); a++)
				full_key += WrittenArgKey(part.arguments[a], subs) + ",";
			full_key += ">";
		}
		string found = subs.Find(full_key);
		if (!found.empty())
		{
			body = found;
			prev_key = full_key;
			continue;
		}
		if (part.kind == NP_IDENTIFIER)
			body += SourceName(part.identifier);
		else
		{
			// The template name is its own substitution candidate; a
			// registered one opens the spelling compressed ("S_IiE").
			string tname = body.empty() ? subs.Find(name_key) : string();
			if (tname.empty())
			{
				subs.Add(name_key);
				tname = SourceName(part.identifier);
			}
			body += tname + "I";
			for (size_t a = 0; a < part.arguments.size(); a++)
				body += MangleWrittenArg(part.arguments[a], subs, 0);
			body += "E";
		}
		subs.Add(full_key);
		prev_key = full_key;
	}
	if (key_out)
		*key_out = prev_key;
	if (name.parts.size() > 1)
		body = "N" + body + "E";
	return body;
}

// The written base type of a specifier sequence: cv + one dependent
// name or one simple-type keyword run; storage/function specifiers
// are skipped (return types reuse declaration specifiers).
string MangleWrittenBase(const vector<const AstSpecifier*>& specifiers,
                         bool keep_cv, Substitutions& subs, string& key)
{
	bool is_const = false;
	bool is_volatile = false;
	const AstName* name = 0;
	string keywords;
	for (size_t i = 0; i < specifiers.size(); i++)
	{
		const AstSpecifier& specifier = *specifiers[i];
		if (specifier.kind == SPEC_TYPE_NAME && !name)
		{
			name = &specifier.name;
			continue;
		}
		if (specifier.kind != SPEC_KEYWORD)
			throw OutsideBoundary("mangled dependent type form");
		if (specifier.keyword == KW_CONST)
			is_const = true;
		else if (specifier.keyword == KW_VOLATILE)
			is_volatile = true;
		else
			keywords += (keywords.empty() ? string() : string(" ")) +
				specifier.spelling;
	}
	string body;
	if (name && keywords.empty())
		body = MangleDependentName(*name, subs, &key);
	else if (!name && !keywords.empty())
	{
		body = FundamentalKeywordCode(keywords);
		key = body;
	}
	else
		throw OutsideBoundary("mangled dependent type form");
	if (keep_cv && (is_const || is_volatile))
	{
		string cv = string(is_volatile ? "V" : "") +
			(is_const ? "K" : "");
		key = cv + "|" + key;
		body = MangleSubstitutable(key, cv + body, subs);
	}
	return body;
}

vector<const AstSpecifier*> TypeSpecifiers(const AstSpecifierSeq& seq)
{
	// Storage-class/function specifiers do not shape the type.
	static const char* const skipped[] = {
		"static", "extern", "inline", "constexpr", "friend", "typedef",
		"virtual", "explicit", "mutable", "thread_local", "register",
	};
	vector<const AstSpecifier*> kept;
	for (size_t i = 0; i < seq.size(); i++)
	{
		bool skip = false;
		if (seq[i].kind == SPEC_KEYWORD)
			for (size_t s = 0; s < sizeof(skipped) / sizeof(skipped[0]);
			     s++)
				if (seq[i].spelling == skipped[s])
					skip = true;
		if (!skip)
			kept.push_back(&seq[i]);
	}
	return kept;
}

// The pointer/reference/cv wrap marks of a written declarator, in
// application order; DI_ID and `...` markers drop out (the caller
// carries the pack flag), any other item is outside the subset.
vector<string> WrittenOps(const AstDeclarator& declarator)
{
	vector<string> ops;
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_ID || item.kind == DI_PACK)
			continue;
		if (item.kind == DI_PTR)
		{
			if (item.spelling == "*")
				ops.push_back("P");
			else if (item.spelling == "&")
				ops.push_back("R");
			else if (item.spelling == "&&")
				ops.push_back("O");
			else
				throw OutsideBoundary("mangled dependent type form");
		}
		else if (item.kind == DI_CV)
			ops.push_back(item.spelling == "volatile" ? "V" : "K");
		else
			throw OutsideBoundary("mangled dependent type form");
	}
	return ops;
}

void ApplyWrittenOps(const vector<string>& ops, Substitutions& subs,
                     string& body, string& key)
{
	for (size_t i = 0; i < ops.size(); i++)
	{
		key = ops[i] + "|" + key;
		body = MangleSubstitutable(key, ops[i] + body, subs);
	}
}

string MangleWrittenTypeId(const AstTypeId& id, Substitutions& subs,
                           string* key_out)
{
	string key;
	string body = MangleWrittenBase(TypeSpecifiers(id.specifiers), true,
	                                subs, key);
	if (id.declarator)
		ApplyWrittenOps(WrittenOps(*id.declarator), subs, body, key);
	if (key_out)
		*key_out = key;
	return body;
}

// A written function parameter, with the 8.3.5p5 adjustment: cv that
// ends up top-level (no enclosing pointer/reference) drops; a pack
// parameter spells `Dp<element>`.
string MangleWrittenParameter(const AstParameter& parameter, bool is_pack,
                              Substitutions& subs)
{
	vector<string> ops;
	if (parameter.declarator)
		ops = WrittenOps(*parameter.declarator);
	while (!ops.empty() && (ops.back() == "K" || ops.back() == "V"))
		ops.pop_back();
	bool keep_cv = false;
	for (size_t i = 0; i < ops.size(); i++)
		if (ops[i] == "P" || ops[i] == "R" || ops[i] == "O")
			keep_cv = true;
	string key;
	string body = MangleWrittenBase(TypeSpecifiers(parameter.specifiers),
	                                keep_cv, subs, key);
	ApplyWrittenOps(ops, subs, body, key);
	if (is_pack)
		body = MangleSubstitutable("Dp|" + key, "Dp" + body, subs);
	return body;
}

// The written return type: declaration specifiers plus the prefix
// pointer/reference/cv items of the pattern declarator.
string MangleWrittenReturn(const AstDecl& inner, Substitutions& subs)
{
	const AstDeclarator* declarator = WrittenDeclarator(inner);
	if (!declarator)
		throw OutsideBoundary("mangled dependent signature form");
	string key;
	string body = MangleWrittenBase(TypeSpecifiers(inner.specifiers), true,
	                                subs, key);
	vector<string> ops;
	for (size_t i = 0; i < declarator->items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator->items[i];
		if (item.kind == DI_ID || item.kind == DI_PARAMS)
			break;
		if (item.kind != DI_PTR && item.kind != DI_CV)
			throw OutsideBoundary("mangled dependent signature form");
		if (item.kind == DI_PTR)
			ops.push_back(item.spelling == "*"
			                  ? "P"
			                  : item.spelling == "&" ? "R" : "O");
		else
			ops.push_back(item.spelling == "volatile" ? "V" : "K");
	}
	ApplyWrittenOps(ops, subs, body, key);
	return body;
}

// The return-type spelling of a specialization's signature: the typed
// pattern when it composed, otherwise the written form.
string MangleSignatureReturn(const TemplateInfo& tmpl, Substitutions& subs)
{
	if (tmpl.conversion_pattern)
		return MangleType(tmpl.conversion_pattern, subs);
	if (tmpl.pattern)
		return MangleType(tmpl.pattern->target, subs);
	if (tmpl.return_pattern)
		return MangleType(tmpl.return_pattern, subs);
	if (!tmpl.pattern_decl)
		throw OutsideBoundary("mangled dependent signature form");
	return MangleWrittenReturn(*tmpl.pattern_decl, subs);
}

// The parameter spellings of a specialization's signature: per
// parameter, the typed pattern (adjusted) or the written form.
string MangleSignatureParameters(const TemplateInfo& tmpl,
                                 Substitutions& subs)
{
	if (tmpl.pattern)
		return MangleBareParameters(tmpl.pattern, subs);
	if (!tmpl.pattern_decl)
		throw OutsideBoundary("mangled dependent signature form");
	const AstDeclarator* declarator = WrittenDeclarator(*tmpl.pattern_decl);
	const AstParameterClause* clause =
		declarator ? WrittenParameterClause(*declarator) : 0;
	if (!clause || clause->parameters.size() != tmpl.param_patterns.size())
		throw OutsideBoundary("mangled dependent signature form");
	string out;
	for (size_t i = 0; i < clause->parameters.size(); i++)
	{
		bool pack = i < tmpl.param_pattern_packs.size() &&
			tmpl.param_pattern_packs[i];
		if (tmpl.param_patterns[i])
		{
			string key;
			string one = MangleType(
				AdjustParameterType(tmpl.param_patterns[i]), subs, &key);
			if (pack)
				one = MangleSubstitutable("Dp|" + key, "Dp" + one, subs);
			out += one;
		}
		else
			out += MangleWrittenParameter(clause->parameters[i], pack,
			                              subs);
	}
	if (clause->variadic)
		out += "z";
	return out.empty() ? string("v") : out;
}

// 5.1.2/5.1.8: <name> I <args> E with the return type included. With
// `written`, the signature spells the as-written pattern; without, it
// reproduces the pre-PA22 concrete fallback (the total spelling for
// forms outside the written subset).
string MangleFunctionTemplateSpelled(const FunctionSpecialization& spec,
                                     bool written)
{
	const TemplateInfo& tmpl = *spec.owner;
	Substitutions subs;
	subs.pattern_params = &tmpl.params;
	vector<NameComponent> parts = ScopeComponents(tmpl.declaring);
	// PA21 member function templates: the encoding is the member form
	// (method cv/ref-qualifiers before the class prefix).
	bool member = tmpl.declaring && tmpl.declaring->kind == SCOPE_CLASS;
	TypePtr pattern = tmpl.pattern ? tmpl.pattern : spec.type;
	if (!written)
		// A pattern with an unexpanded pack parameter mangles from the
		// concrete signature in the fallback spelling.
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
	// A conversion-function template's terminal encodes `cv` with the
	// abstract conversion pattern; the return type re-spells it.
	string terminal = tmpl.conversion_pattern
		? "cv" + MangleType(tmpl.conversion_pattern, subs)
		: MangleTerminalName(tmpl.name, pattern->parameters.size() +
		                                (member ? 1 : 0));
	size_t pack_start;
	size_t pack_end;
	ArgsPackSpan(tmpl.params, spec.args.size(), pack_start, pack_end);
	string targs = MangleArgList(spec.args, pack_start, pack_end, subs);
	string result;
	string params;
	if (written)
	{
		result = MangleSignatureReturn(tmpl, subs);
		params = MangleSignatureParameters(tmpl, subs);
	}
	else
	{
		result = MangleType(tmpl.conversion_pattern
		                        ? tmpl.conversion_pattern
		                        : pattern->target,
		                    subs);
		params = MangleBareParameters(pattern, subs);
	}
	string encoding = terminal + "I" + targs + "E";
	if (!parts.empty() || member)
		encoding = "N" + cv + prefix + encoding + "E";
	return "_Z" + encoding + result + params;
}

}  // namespace

string MangleDependentTypeId(const AstTypeId& id, Substitutions& subs,
                             string* key_out)
{
	return MangleWrittenTypeId(id, subs, key_out);
}

}  // namespace lower_mangle

using namespace lower_mangle;

string MangleFunctionTemplateObjectName(const FunctionSpecialization& spec)
{
	try
	{
		return MangleFunctionTemplateSpelled(spec, true);
	}
	catch (const std::exception&)
	{
		// Forms outside the written-signature subset keep the total
		// concrete spelling (`object=` stays a pairing hint).
		return MangleFunctionTemplateSpelled(spec, false);
	}
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
	subs.pattern_params = &tmpl.params;
	vector<NameComponent> parts = ScopeComponents(scope);
	string encoding = "N";
	ManglePrefixComponents(parts, subs, encoding);
	TypePtr pattern = tmpl.pattern ? tmpl.pattern : spec.type;
	size_t pack_start;
	size_t pack_end;
	ArgsPackSpan(tmpl.params, spec.args.size(), pack_start, pack_end);
	string targs = MangleArgList(spec.args, pack_start, pack_end, subs);
	encoding += special_code + "I" + targs + "E";
	encoding += "E";
	return "_Z" + encoding + MangleBareParameters(pattern, subs);
}
