#include "lowering/lower_name.h"

#include <stdexcept>
#include <vector>

#include "ast/ast.h"
#include "lowering/lower_name_parts.h"

using std::to_string;
using std::vector;

// PA22 function-template signature encodings (5.1.2/5.1.8): the
// <name> I <args> E spelling of a specialization with its return type
// and parameters, spelled from the as-written pattern when the form
// composes and from the concrete fallback otherwise. The written-form
// machinery itself lives in lower_name_template.cpp; this file owns
// the signature assembly and the public object-name entry points.

namespace lower_mangle {

namespace {

// Whether a composed pattern type mentions an alias-template
// specialization. 14.5.7p2: such spellings must never reach a mangled
// name; the written form expands them to the aliased type.
bool TypeMentionsAliasSpec(const TypePtr& type)
{
	if (!type)
		return false;
	if (type->named && type->named->spec_template &&
	    type->named->spec_template->kind == TMPL_ALIAS)
		return true;
	if (TypeMentionsAliasSpec(type->target))
		return true;
	for (size_t i = 0; i < type->parameters.size(); i++)
		if (TypeMentionsAliasSpec(type->parameters[i]))
			return true;
	return false;
}

// The return-type spelling of a specialization's signature: the typed
// pattern when it composed, otherwise the written form.
string MangleSignatureReturn(const TemplateInfo& tmpl, Substitutions& subs)
{
	if (tmpl.conversion_pattern)
		return MangleType(tmpl.conversion_pattern, subs);
	// A trailing decltype return spells its written expression
	// (DT..E); the composed pattern carries only the auto stand-in.
	if (tmpl.pattern_decl)
		if (const AstDeclarator* declarator =
		        WrittenDeclarator(*tmpl.pattern_decl))
			for (size_t i = 0; i < declarator->items.size(); i++)
			{
				const AstDeclaratorItem& item = declarator->items[i];
				if (item.kind != DI_TRAILING_RETURN ||
				    !item.trailing_type)
					continue;
				const AstTypeId& trailing = *item.trailing_type;
				if (trailing.specifiers.size() == 1 &&
				    trailing.specifiers[0].kind == SPEC_DECLTYPE)
					return MangleDependentTypeId(trailing, subs, 0);
				break;
			}
	if (tmpl.pattern && !TypeMentionsAliasSpec(tmpl.pattern->target))
		return MangleType(tmpl.pattern->target, subs);
	if (tmpl.return_pattern && !TypeMentionsAliasSpec(tmpl.return_pattern))
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
                                     bool written, Substitutions& subs)
{
	const TemplateInfo& tmpl = *spec.owner;
	subs.pattern_params = &tmpl.params;
	// Written names resolve against the template's lookup scope, so
	// alias template-ids expand transparently (14.5.7p2).
	subs.written_scope = TemplateLookupScope(tmpl);
	// The written parameter names: decltype-return expressions spell
	// fp_/fp<n-1>_ references through them.
	vector<string> written_param_names;
	if (tmpl.pattern_decl)
		if (const AstDeclarator* fn_declarator =
		        WrittenDeclarator(*tmpl.pattern_decl))
			if (const AstParameterClause* fn_clause =
			        WrittenParameterClause(*fn_declarator))
				for (size_t i = 0; i < fn_clause->parameters.size(); i++)
				{
					const AstParameter& parameter =
						fn_clause->parameters[i];
					const AstName* id = parameter.declarator
						? parameter.declarator->IdName() : 0;
					written_param_names.push_back(
						id && id->IsPlainIdentifier()
							? id->parts[0].identifier : string());
				}
	subs.fn_param_names = &written_param_names;
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
	// The template-name (prefix plus terminal) is itself a
	// substitution candidate: a re-encounter in the same mangled
	// name compresses wholesale (`ZNS7_IS4_SE_EE...`).
	string key_chain;
	for (size_t i = 0; i < parts.size(); i++)
	{
		string part_name_key;
		string part_full_key;
		ComponentKeys(parts[i], key_chain, subs.pattern_params,
		              part_name_key, part_full_key);
		key_chain = part_full_key;
	}
	string name_key = (key_chain.empty() ? string("T:")
	                                     : key_chain + "::") +
		tmpl.name;
	string name_sub = tmpl.conversion_pattern ? string()
	                                          : subs.Find(name_key);
	// 5.1.4.2: a function template directly in ::std keeps the
	// unscoped-template-name St form (no nested-name); the template
	// name is still a substitution candidate.
	bool unscoped_std = parts.size() == 1 && HeadIsStd(parts) &&
		!member && !tmpl.conversion_pattern;
	string prefix;
	string terminal;
	if (!name_sub.empty())
		terminal = name_sub;
	else if (unscoped_std)
	{
		subs.Add(name_key);
		terminal = "St" +
			MangleTerminalName(tmpl.name,
			                   pattern->parameters.size());
	}
	else
	{
		ManglePrefixComponents(parts, subs, prefix);
		subs.Add(name_key);
		// A conversion-function template's terminal encodes `cv`
		// with the abstract conversion pattern; the return type
		// re-spells it.
		terminal = tmpl.conversion_pattern
			? "cv" + MangleType(tmpl.conversion_pattern, subs)
			: MangleTerminalName(tmpl.name,
			                     pattern->parameters.size() +
			                         (member ? 1 : 0));
	}
	size_t pack_start;
	size_t pack_end;
	ArgsPackSpan(tmpl.params, spec.args.size(), pack_start, pack_end);
	string targs;
	for (size_t i = 0; i <= spec.args.size(); i++)
	{
		if (pack_start != (size_t)-1 && i == pack_start)
			targs += "J";
		if (pack_start != (size_t)-1 && i == pack_end &&
		    pack_end >= pack_start)
			targs += "E";
		if (i >= spec.args.size())
			continue;
		// 5.1.5.10: an argument for a non-type parameter whose
		// written declared type is instantiation-dependent carries
		// the parameter declaration (`Tn <type>`).
		size_t param_index = i;
		if (pack_start != (size_t)-1 && i >= pack_start)
			param_index = i < pack_end
				? pack_start
				: i - (pack_end - pack_start) + 1;
		if (written && param_index < tmpl.params.size())
		{
			const TemplateParam& param = tmpl.params[param_index];
			if (param.kind == TPK_VALUE && param.source &&
			    WrittenSpecifiersMentionParam(param.source->specifiers,
			                                  tmpl.params))
			{
				vector<string> ops;
				if (param.source->declarator)
					ops = WrittenOps(*param.source->declarator);
				string key;
				string decl_type = MangleWrittenBase(
					TypeSpecifiers(param.source->specifiers), true,
					subs, key);
				ApplyWrittenOps(ops, subs, decl_type, key);
				targs += "Tn" + decl_type;
			}
		}
		targs += MangleTemplateArg(spec.args[i], subs);
	}
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
	if ((!parts.empty() && !unscoped_std) || member)
		encoding = "N" + cv + prefix + encoding + "E";
	return "_Z" + encoding + result + params;
}

}  // namespace

}  // namespace lower_mangle

using namespace lower_mangle;

string MangleFunctionTemplateEncoding(const FunctionSpecialization& spec,
                                      Substitutions& subs)
{
	// The embedded encoding shares the outer substitution table; the
	// pattern context switches to this template's parameters for the
	// duration (T_/T<n>_ inside name a different parameter list).
	const vector<TemplateParam>* saved_params = subs.pattern_params;
	const vector<string>* saved_names = subs.fn_param_names;
	const Scope* saved_scope = subs.written_scope;
	string mangled;
	try
	{
		mangled = MangleFunctionTemplateSpelled(spec, true, subs);
	}
	catch (const std::exception&)
	{
		subs.pattern_params = saved_params;
		subs.fn_param_names = saved_names;
		subs.written_scope = saved_scope;
		// Forms outside the written-signature subset keep the
		// isolated concrete spelling as a pairing hint; the outer
		// table stays as-is.
		Substitutions isolated;
		mangled = MangleFunctionTemplateSpelled(spec, false, isolated);
	}
	subs.pattern_params = saved_params;
	subs.fn_param_names = saved_names;
	subs.written_scope = saved_scope;
	return mangled.compare(0, 2, "_Z") == 0 ? mangled.substr(2)
	                                        : mangled;
}

string MangleFunctionTemplateObjectName(const FunctionSpecialization& spec)
{
	string mangled;
	try
	{
		Substitutions subs;
		mangled = MangleFunctionTemplateSpelled(spec, true, subs);
	}
	catch (const std::exception&)
	{
		// Forms outside the written-signature subset keep the total
		// concrete spelling (`object=` stays a pairing hint).
		Substitutions subs;
		mangled = MangleFunctionTemplateSpelled(spec, false, subs);
	}
	// PA24: concrete spellings that embed a lambda-owning local scope
	// can carry punctuation the LowIR metadata grammar rejects; the
	// pairing hint sanitizes to identifier characters.
	for (size_t i = 0; i < mangled.size(); i++)
	{
		char c = mangled[i];
		bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
		if (!word)
			mangled[i] = '_';
	}
	return mangled;
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
