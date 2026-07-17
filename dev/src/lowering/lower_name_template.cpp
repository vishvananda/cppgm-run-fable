#include "lowering/lower_name.h"

#include <stdexcept>
#include <cstdio>
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

// --- written-form dependency scan -------------------------------------
// Whether a written construct mentions one of the template's own
// parameter names (the head's names shadow outer bindings, so a
// matching identifier is a parameter reference). Drives the
// 5.1.5.10 `Tn <type>` prefix for arguments of non-type parameters
// whose declared type is instantiation-dependent.

bool WrittenExprMentionsParam(const AstExpr& expr,
                              const vector<TemplateParam>& params);
bool WrittenTypeIdMentionsParam(const AstTypeId& id,
                                const vector<TemplateParam>& params);

bool ParamNameMatches(const string& id,
                      const vector<TemplateParam>& params)
{
	for (size_t p = 0; p < params.size(); p++)
		if (!params[p].name.empty() && params[p].name == id)
			return true;
	return false;
}

bool WrittenNameMentionsParam(const AstName& name,
                              const vector<TemplateParam>& params)
{
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if ((part.kind == NP_IDENTIFIER ||
		     part.kind == NP_TEMPLATE_ID) &&
		    ParamNameMatches(part.identifier, params))
			return true;
		for (size_t a = 0; a < part.arguments.size(); a++)
		{
			const AstTemplateArgument& arg = part.arguments[a];
			if (arg.is_type && arg.type &&
			    WrittenTypeIdMentionsParam(*arg.type, params))
				return true;
			if (!arg.is_type && arg.expr &&
			    WrittenExprMentionsParam(*arg.expr, params))
				return true;
		}
		if (part.decltype_expr &&
		    WrittenExprMentionsParam(*part.decltype_expr, params))
			return true;
	}
	return false;
}

bool WrittenExprMentionsParam(const AstExpr& expr,
                              const vector<TemplateParam>& params)
{
	if (WrittenNameMentionsParam(expr.name, params))
		return true;
	if (expr.type && WrittenTypeIdMentionsParam(*expr.type, params))
		return true;
	for (size_t i = 0; i < expr.operands.size(); i++)
		if (expr.operands[i] &&
		    WrittenExprMentionsParam(*expr.operands[i], params))
			return true;
	for (size_t i = 0; i < expr.arguments.size(); i++)
		if (expr.arguments[i] &&
		    WrittenExprMentionsParam(*expr.arguments[i], params))
			return true;
	return false;
}

bool WrittenSpecifiersMentionParam(const AstSpecifierSeq& seq,
                                   const vector<TemplateParam>& params)
{
	for (size_t i = 0; i < seq.size(); i++)
	{
		const AstSpecifier& spec = seq[i];
		if (spec.kind == SPEC_TYPE_NAME &&
		    WrittenNameMentionsParam(spec.name, params))
			return true;
		if (spec.kind == SPEC_DECLTYPE && spec.decltype_expr &&
		    WrittenExprMentionsParam(*spec.decltype_expr, params))
			return true;
	}
	return false;
}

bool WrittenTypeIdMentionsParam(const AstTypeId& id,
                                const vector<TemplateParam>& params)
{
	// Declarator ops (*, &, cv) carry no names in the written subset.
	return WrittenSpecifiersMentionParam(id.specifiers, params);
}

// --- alias-template transparency (14.5.7p2) ----------------------------
// An alias template-id is equivalent to the aliased type, so it never
// reaches a mangled name: the written machinery expands it in place,
// binding the alias's parameter names to the use site's written
// arguments through Substitutions::AliasFrame.

string MangleWrittenTypeId(const AstTypeId& id, Substitutions& subs,
                           string* key_out);
string MangleWrittenExprNode(const AstExpr& expr, Substitutions& subs);

// RAII spelling-context save/restore around splices and expansions.
struct MangleContext
{
	explicit MangleContext(Substitutions& subs)
		: subs(subs), params(subs.pattern_params),
		  fn_params(subs.fn_param_names), scope(subs.written_scope),
		  frame(subs.alias_frame)
	{
	}
	~MangleContext()
	{
		subs.pattern_params = params;
		subs.fn_param_names = fn_params;
		subs.written_scope = scope;
		subs.alias_frame = frame;
	}
	Substitutions& subs;
	const vector<TemplateParam>* params;
	const vector<string>* fn_params;
	const Scope* scope;
	const Substitutions::AliasFrame* frame;
};

// The position of `id` among the active alias frame's parameters,
// -1 when the frame does not bind it.
int AliasParamIndex(const Substitutions& subs, const string& id)
{
	if (!subs.alias_frame || !subs.alias_frame->alias)
		return -1;
	const vector<TemplateParam>& params =
		subs.alias_frame->alias->params;
	for (size_t p = 0; p < params.size(); p++)
		if (!params[p].name.empty() && params[p].name == id)
			return (int)p;
	return -1;
}

// The alias template `id` names in the written lookup scope, if any.
// Template parameters and alias-frame bindings shadow it.
const TemplateInfo* FindAliasTemplate(const Substitutions& subs,
                                      const string& id)
{
	if (AliasParamIndex(subs, id) >= 0 ||
	    TemplateParamIndex(subs, TPK_TYPE, id) >= 0 ||
	    TemplateParamIndex(subs, TPK_TEMPLATE, id) >= 0)
		return 0;
	for (const Scope* scope = subs.written_scope; scope;
	     scope = scope->parent)
		if (const ScopeBinding* binding = FindOwnBinding(*scope, id))
		{
			if (binding->kind == SB_CLASS_TEMPLATE && binding->templ &&
			    binding->templ->kind == TMPL_ALIAS)
				return binding->templ;
			return 0;   // shadowed by a non-alias meaning
		}
	return 0;
}

// Any class/alias template `id` names in the written lookup scope
// (parameters and frame bindings shadow it) - the argument spellings
// consult its parameter kinds.
const TemplateInfo* FindNamedTemplate(const Substitutions& subs,
                                      const string& id)
{
	if (AliasParamIndex(subs, id) >= 0 ||
	    TemplateParamIndex(subs, TPK_TYPE, id) >= 0 ||
	    TemplateParamIndex(subs, TPK_TEMPLATE, id) >= 0)
		return 0;
	for (const Scope* scope = subs.written_scope; scope;
	     scope = scope->parent)
		if (const ScopeBinding* binding = FindOwnBinding(*scope, id))
			return binding->kind == SB_CLASS_TEMPLATE ? binding->templ
			                                          : 0;
	return 0;
}

// Mangles the type bound to alias parameter `index`: the use site's
// written argument in the use site's own context, or the alias's own
// default (written in the alias head, so the frame stays active).
string MangleAliasBoundType(Substitutions& subs, int index,
                            string* key_out)
{
	const Substitutions::AliasFrame& frame = *subs.alias_frame;
	MangleContext saved(subs);
	// A typed pattern expansion splices the resolved argument type in
	// the enclosing (pattern) context.
	if (frame.typed_args)
	{
		if ((size_t)index >= frame.typed_args->size())
		{
			const AstTypeId* fallback =
				frame.alias->params[(size_t)index].default_type;
			if (!fallback)
				throw OutsideBoundary("mangled dependent type form");
			return MangleWrittenTypeId(*fallback, subs, key_out);
		}
		const TemplateArg& arg = (*frame.typed_args)[index];
		if (arg.is_value || !arg.type)
			throw OutsideBoundary("mangled dependent type form");
		subs.pattern_params = frame.outer_params;
		subs.fn_param_names = frame.outer_fn_params;
		subs.written_scope = frame.outer_scope;
		subs.alias_frame = frame.outer;
		return MangleType(arg.type, subs, key_out);
	}
	const AstTypeId* type = 0;
	if ((size_t)index < frame.args->size())
	{
		const AstTemplateArgument& arg = (*frame.args)[index];
		if (!arg.is_type || !arg.type)
			throw OutsideBoundary("mangled dependent type form");
		type = arg.type.get();
		subs.pattern_params = frame.outer_params;
		subs.fn_param_names = frame.outer_fn_params;
		subs.written_scope = frame.outer_scope;
		subs.alias_frame = frame.outer;
	}
	else
	{
		type = frame.alias->params[(size_t)index].default_type;
		if (!type)
			throw OutsideBoundary("mangled dependent type form");
	}
	return MangleWrittenTypeId(*type, subs, key_out);
}

// Mangles the expression bound to alias parameter `index` as a bare
// expression node (spliced inside a larger written expression).
string MangleAliasBoundExprNode(Substitutions& subs, int index)
{
	const Substitutions::AliasFrame& frame = *subs.alias_frame;
	MangleContext saved(subs);
	// A typed pattern expansion splices the resolved value argument:
	// a forwarded value parameter spells T_/Tn_ in the enclosing
	// context; a deferred dependent expression re-spells as written.
	if (frame.typed_args)
	{
		if ((size_t)index >= frame.typed_args->size())
			throw OutsideBoundary("mangled dependent expression form");
		const TemplateArg& arg = (*frame.typed_args)[index];
		if (!arg.is_value)
			throw OutsideBoundary("mangled dependent expression form");
		subs.pattern_params = frame.outer_params;
		subs.fn_param_names = frame.outer_fn_params;
		subs.written_scope = frame.outer_scope;
		subs.alias_frame = frame.outer;
		if (arg.value_param >= 0)
		{
			string key = "TP:" + PointerKey(subs.pattern_params) + ":" +
				to_string(arg.value_param);
			string found = subs.FindParam(key);
			if (!found.empty())
				return found;
			subs.AddParam(key);
			return arg.value_param == 0
				? string("T_")
				: "T" + to_string(arg.value_param - 1) + "_";
		}
		if (arg.dependent_value)
			return MangleWrittenExprNode(*arg.dependent_value, subs);
		throw OutsideBoundary("mangled dependent expression form");
	}
	const AstExpr* expr = 0;
	if ((size_t)index < frame.args->size())
	{
		const AstTemplateArgument& arg = (*frame.args)[index];
		if (arg.is_type || !arg.expr)
			throw OutsideBoundary("mangled dependent expression form");
		expr = arg.expr.get();
		subs.pattern_params = frame.outer_params;
		subs.fn_param_names = frame.outer_fn_params;
		subs.written_scope = frame.outer_scope;
		subs.alias_frame = frame.outer;
	}
	else
	{
		expr = frame.alias->params[(size_t)index].default_expr;
		if (!expr)
			throw OutsideBoundary("mangled dependent expression form");
	}
	return MangleWrittenExprNode(*expr, subs);
}

// Expands one alias template-id: the aliased type-id mangles with the
// alias's parameters bound to the written arguments. The key is the
// expansion's own key, so a directly written spelling of the same
// type unifies with it.
string MangleAliasExpansion(const TemplateInfo& alias,
                            const vector<AstTemplateArgument>& args,
                            Substitutions& subs, string* key_out)
{
	if (!alias.alias_type || args.size() > alias.params.size())
		throw OutsideBoundary("mangled dependent type form");
	Substitutions::AliasFrame frame;
	frame.alias = &alias;
	frame.args = &args;
	frame.outer_params = subs.pattern_params;
	frame.outer_fn_params = subs.fn_param_names;
	frame.outer_scope = subs.written_scope;
	frame.outer = subs.alias_frame;
	MangleContext saved(subs);
	subs.alias_frame = &frame;
	subs.pattern_params = 0;
	subs.fn_param_names = 0;
	subs.written_scope = TemplateLookupScope(alias);
	return MangleWrittenTypeId(*alias.alias_type, subs, key_out);
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

string MangleWrittenBase(const vector<const AstSpecifier*>& specifiers,
                         bool keep_cv, Substitutions& subs, string& key);
vector<const AstSpecifier*> TypeSpecifiers(const AstSpecifierSeq& seq);
vector<string> WrittenOps(const AstDeclarator& declarator);
void ApplyWrittenOps(const vector<string>& ops, Substitutions& subs,
                     string& body, string& key);
string MangleWrittenArg(const AstTemplateArgument& argument,
                        Substitutions& subs, string* key_out);
string MangleWrittenExprNode(const AstExpr& expr, Substitutions& subs);

// The unresolved callee of a written call: `A<T>::b` spells
// `sr <prefix-template-id> E <member source-name>`; an unqualified
// (possibly template-id) callee spells the source-name with its
// written arguments. Unresolved names are not substitution
// candidates (the checked encodings).
string MangleWrittenCallee(const AstName& name, Substitutions& subs)
{
	if (name.parts.empty() || name.global_scope)
		throw OutsideBoundary("mangled dependent expression form");
	string out;
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		bool terminal = i + 1 == name.parts.size();
		if (part.kind != NP_IDENTIFIER && part.kind != NP_TEMPLATE_ID)
			throw OutsideBoundary("mangled dependent expression form");
		if (!terminal && i == 0)
			out += "sr";
		string spelled = SourceName(part.identifier);
		if (part.kind == NP_TEMPLATE_ID)
		{
			spelled += "I";
			for (size_t a = 0; a < part.arguments.size(); a++)
				spelled += MangleWrittenArg(part.arguments[a], subs, 0);
			spelled += "E";
		}
		if (!terminal)
		{
			if (part.kind != NP_TEMPLATE_ID)
				throw OutsideBoundary(
					"mangled dependent expression form");
			spelled += "E";
		}
		out += spelled;
	}
	return out;
}

// A written expression inside a decltype return (the reference
// conventions): calls spell `cl`, static_cast spells `sc` with the
// target's base type (its reference wrap drops), function-parameter
// references spell fp_/fp<n-1>_, and template parameters compress
// like written types.
string MangleWrittenExprNode(const AstExpr& expr, Substitutions& subs)
{
	switch (expr.kind)
	{
	case EK_PAREN:
		return MangleWrittenExprNode(*expr.operands[0], subs);
	case EK_ID:
	{
		if (expr.name.parts.size() == 1 &&
		    expr.name.parts[0].kind == NP_IDENTIFIER)
		{
			const string& id = expr.name.parts[0].identifier;
			int alias_index = AliasParamIndex(subs, id);
			if (alias_index >= 0)
				return MangleAliasBoundExprNode(subs, alias_index);
			if (subs.fn_param_names)
				for (size_t i = 0; i < subs.fn_param_names->size(); i++)
					if ((*subs.fn_param_names)[i] == id)
						return i == 0
							? string("fp_")
							: "fp" + to_string(i - 1) + "_";
			int index = TemplateParamIndex(subs, TPK_VALUE, id);
			if (index >= 0)
			{
				string key = "TP:" +
					PointerKey(subs.pattern_params) + ":" +
					to_string(index);
				string found = subs.FindParam(key);
				if (!found.empty())
					return found;
				subs.AddParam(key);
				return ParamSpelling(index);
			}
		}
		// A dependent qualified id (`A<T>::value`) spells the
		// unresolved sr form; its qualifier levels are not
		// substitution candidates (5.1.6).
		if (expr.name.parts.size() > 1)
			return MangleWrittenCallee(expr.name, subs);
		throw OutsideBoundary("mangled dependent expression form");
	}
	case EK_CALL:
	{
		const AstExpr* callee = expr.operands[0].get();
		while (callee->kind == EK_PAREN)
			callee = callee->operands[0].get();
		string out = "cl";
		if (callee->kind == EK_ID)
			out += MangleWrittenCallee(callee->name, subs);
		else if (callee->kind == EK_MEMBER)
			out += MangleWrittenExprNode(*callee, subs);
		else
			throw OutsideBoundary("mangled dependent expression form");
		for (size_t i = 0; i < expr.arguments.size(); i++)
			out += MangleWrittenExprNode(*expr.arguments[i], subs);
		return out + "E";
	}
	case EK_BINARY:
	{
		string code;
		if (expr.operands.size() != 2 ||
		    !LookupOperatorCode(expr.op_spelling, code))
			throw OutsideBoundary("mangled dependent expression form");
		return code + MangleWrittenExprNode(*expr.operands[0], subs) +
			MangleWrittenExprNode(*expr.operands[1], subs);
	}
	case EK_UNARY:
	{
		// 5.1.2: the unary forms of the dual-meaning operators carry
		// their own codes.
		if (expr.operands.size() != 1)
			throw OutsideBoundary("mangled dependent expression form");
		string code = expr.op_spelling == "*" ? "de"
			: expr.op_spelling == "&" ? "ad"
			: expr.op_spelling == "+" ? "ps"
			: expr.op_spelling == "-" ? "ng"
			: expr.op_spelling == "!" ? "nt"
			: expr.op_spelling == "~" ? "co"
			: string();
		if (code.empty())
			throw OutsideBoundary("mangled dependent expression form");
		return code + MangleWrittenExprNode(*expr.operands[0], subs);
	}
	case EK_MEMBER:
	{
		// 5.1.6: class member access spells `dt`/`pt` with the object
		// expression and the member's source-name.
		if (expr.name.parts.size() != 1 ||
		    expr.name.parts[0].kind != NP_IDENTIFIER ||
		    expr.name.parts[0].tilde)
			throw OutsideBoundary("mangled dependent expression form");
		return string(expr.op_spelling == "->" ? "pt" : "dt") +
			MangleWrittenExprNode(*expr.operands[0], subs) +
			SourceName(expr.name.parts[0].identifier);
	}
	case EK_KEYWORD_CAST:
	{
		if (expr.op != KW_STATIC_CAST || !expr.type)
			throw OutsideBoundary("mangled dependent expression form");
		string key;
		string body = MangleWrittenBase(
			TypeSpecifiers(expr.type->specifiers), true, subs, key);
		if (expr.type->declarator)
		{
			vector<string> ops = WrittenOps(*expr.type->declarator);
			while (!ops.empty() &&
			       (ops.back() == "R" || ops.back() == "O"))
				ops.pop_back();
			ApplyWrittenOps(ops, subs, body, key);
		}
		return "sc" + body +
			MangleWrittenExprNode(*expr.operands[0], subs);
	}
	default:
		throw OutsideBoundary("mangled dependent expression form");
	}
}

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
	// A general written expression argument (dependent qualified ids,
	// calls, casts) spells `X <expression> E`.
	string body = MangleWrittenExprNode(expr, subs);
	if (key_out)
	{
		// Structural key: re-spell against a fresh table so live
		// substitution state never leaks into enclosing keys.
		Substitutions throwaway;
		throwaway.pattern_params = subs.pattern_params;
		throwaway.fn_param_names = subs.fn_param_names;
		throwaway.written_scope = subs.written_scope;
		throwaway.alias_frame = subs.alias_frame;
		*key_out = "XE:" + MangleWrittenExprNode(expr, throwaway);
	}
	return "X" + body + "E";
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

// The bare qualified-name of a type-id with no declarator operations
// and exactly one unqualified type-name specifier, or null.
const AstName* BareTypeIdName(const AstTypeId& id)
{
	if (id.declarator && !id.declarator->Empty())
		return 0;
	vector<const AstSpecifier*> specifiers = TypeSpecifiers(id.specifiers);
	if (specifiers.size() != 1 ||
	    specifiers[0]->kind != SPEC_TYPE_NAME ||
	    specifiers[0]->name.typename_keyword ||
	    specifiers[0]->name.global_scope)
		return 0;
	return &specifiers[0]->name;
}

// A written argument in a value-parameter position: an argument the
// type-preferred parse committed as a bare qualified-name re-reads as
// its expression spelling (`X sr..E` - the parameter kind decides the
// ambiguity the parse could not).
string MangleWrittenValueArg(const AstTemplateArgument& argument,
                             Substitutions& subs, string* key_out)
{
	const AstName* name = argument.is_type && argument.type
		? BareTypeIdName(*argument.type) : 0;
	if (!name)
		return MangleWrittenArg(argument, subs, key_out);
	string body = MangleWrittenCallee(*name, subs);
	if (key_out)
	{
		Substitutions throwaway;
		throwaway.pattern_params = subs.pattern_params;
		throwaway.fn_param_names = subs.fn_param_names;
		throwaway.written_scope = subs.written_scope;
		throwaway.alias_frame = subs.alias_frame;
		*key_out = "XE:" + MangleWrittenCallee(*name, throwaway);
	}
	return "X" + body + "E";
}

// The structural key of one written argument (aligned with the typed
// ArgKey format), independent of the live substitution state.
string WrittenArgKey(const AstTemplateArgument& argument,
                     const Substitutions& subs,
                     bool value_position = false)
{
	Substitutions throwaway;
	throwaway.pattern_params = subs.pattern_params;
	throwaway.fn_param_names = subs.fn_param_names;
	throwaway.written_scope = subs.written_scope;
	throwaway.alias_frame = subs.alias_frame;
	string key;
	if (value_position)
		MangleWrittenValueArg(argument, throwaway, &key);
	else
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
	// 14.5.7p2: a written alias template-id is the aliased type.
	if (name.parts.size() == 1 &&
	    name.parts[0].kind == NP_TEMPLATE_ID && !name.parts[0].tilde)
		if (const TemplateInfo* alias =
		        FindAliasTemplate(subs, name.parts[0].identifier))
			return MangleAliasExpansion(*alias,
			                            name.parts[0].arguments, subs,
			                            key_out);
	if (name.parts.empty())
		throw OutsideBoundary("mangled dependent name form");
	string body;
	string prev_key;
	bool prefixed = false;
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if ((part.kind != NP_IDENTIFIER && part.kind != NP_TEMPLATE_ID) ||
		    part.tilde)
			throw OutsideBoundary("mangled dependent name form");
		if (i == 0 && part.kind == NP_IDENTIFIER)
		{
			// An alias parameter splices the use site's written
			// argument (spelled in the use site's context).
			int alias_index = AliasParamIndex(subs, part.identifier);
			if (alias_index >= 0)
			{
				if (name.parts.size() != 1)
					throw OutsideBoundary(
						"mangled dependent type form");
				return MangleAliasBoundType(subs, alias_index,
				                            key_out);
			}
			// A leading template-parameter reference.
			int index = TemplateParamIndex(subs, TPK_TYPE,
			                               part.identifier);
			if (index >= 0)
			{
				prev_key = "TP:" +
					PointerKey(subs.pattern_params) + ":" +
					to_string(index);
				string found = subs.FindParam(prev_key);
				if (!found.empty())
					body = found;
				else
				{
					subs.AddParam(prev_key);
					body = ParamSpelling(index);
				}
				continue;
			}
		}
		// A leading name homed in a named namespace spells its
		// declaring prefix (each level a substitution candidate), so
		// the qualified dependent name unifies with the typed
		// spellings of the same scope (`NS_2IdIT_E4typeE`).
		if (i == 0 && prev_key.empty())
		{
			const Scope* declaring = 0;
			for (const Scope* s = subs.written_scope; s; s = s->parent)
				if (FindOwnBinding(*s, part.identifier))
				{
					declaring = s;
					break;
				}
			if (declaring && declaring->parent &&
			    declaring->kind == SCOPE_NAMESPACE)
			{
				vector<NameComponent> prefix_parts =
					ScopeComponents(declaring);
				if (!prefix_parts.empty())
				{
					prev_key = ManglePrefixComponents(prefix_parts,
					                                  subs, body);
					prefixed = true;
				}
			}
		}
		string name_key = (prev_key.empty() ? string("T:")
		                                    : prev_key + "::") +
			part.identifier;
		string full_key = name_key;
		// The named template's parameter kinds decide each argument's
		// type-versus-expression reading (the parse preferred type).
		const TemplateInfo* named_templ =
			part.kind == NP_TEMPLATE_ID && i == 0
				? FindNamedTemplate(subs, part.identifier) : 0;
		if (part.kind == NP_TEMPLATE_ID)
		{
			full_key += "<";
			for (size_t a = 0; a < part.arguments.size(); a++)
			{
				bool value_position = named_templ &&
					a < named_templ->params.size() &&
					named_templ->params[a].kind == TPK_VALUE;
				full_key += WrittenArgKey(part.arguments[a], subs,
				                          value_position) + ",";
			}
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
			{
				bool value_position = named_templ &&
					a < named_templ->params.size() &&
					named_templ->params[a].kind == TPK_VALUE;
				body += value_position
					? MangleWrittenValueArg(part.arguments[a], subs, 0)
					: MangleWrittenArg(part.arguments[a], subs, 0);
			}
			body += "E";
		}
		subs.Add(full_key);
		prev_key = full_key;
	}
	if (key_out)
		*key_out = prev_key;
	if (name.parts.size() > 1 || prefixed)
		body = "N" + body + "E";
	return body;
}

// The written base type of a specifier sequence: cv + one dependent
// name or one simple-type keyword run; storage/function specifiers
// are skipped (return types reuse declaration specifiers).
string MangleWrittenBase(const vector<const AstSpecifier*>& specifiers,
                         bool keep_cv, Substitutions& subs, string& key)
{
	// decltype(expr) spells the expression (5.1.9 DT..E), a
	// substitution candidate like any other type. The key re-spells
	// against a fresh table so live substitution state never leaks in
	// (two spellings of the same decltype must unify).
	if (specifiers.size() == 1 &&
	    specifiers[0]->kind == SPEC_DECLTYPE &&
	    specifiers[0]->decltype_expr)
	{
		string body = "DT" +
			MangleWrittenExprNode(*specifiers[0]->decltype_expr, subs) +
			"E";
		Substitutions throwaway;
		throwaway.pattern_params = subs.pattern_params;
		throwaway.fn_param_names = subs.fn_param_names;
		throwaway.written_scope = subs.written_scope;
		throwaway.alias_frame = subs.alias_frame;
		key = "DT:" +
			MangleWrittenExprNode(*specifiers[0]->decltype_expr,
			                      throwaway);
		return MangleSubstitutable(key, body, subs);
	}
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

string MangleWrittenParameter(const AstParameter& parameter, bool is_pack,
                              Substitutions& subs, string* key_out = 0);

// Whether a written parameter spells its own `...` expansion.
bool WrittenParameterIsPack(const AstParameter& parameter)
{
	if (!parameter.declarator)
		return false;
	for (size_t i = 0; i < parameter.declarator->items.size(); i++)
		if (parameter.declarator->items[i].kind == DI_PACK)
			return true;
	return false;
}

// A written declarator over the base type: prefix pointer/reference/cv
// marks apply to the base; one parameter clause wraps it as a function
// type (`F <return> <parameters> E`, a substitution candidate), with a
// nested declarator's marks (the `(*)` of a pointer-to-function)
// applying after the wrap.
void ApplyWrittenDeclarator(const AstDeclarator& declarator,
                            Substitutions& subs, string& body,
                            string& key)
{
	vector<string> prefix;
	const AstParameterClause* clause = 0;
	vector<string> tail;
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_ID || item.kind == DI_PACK)
			continue;
		if (item.kind == DI_PTR || item.kind == DI_CV)
		{
			if (clause)
				throw OutsideBoundary("mangled dependent type form");
			if (item.kind == DI_PTR)
				prefix.push_back(item.spelling == "*"
				                     ? "P"
				                     : item.spelling == "&" ? "R" : "O");
			else
				prefix.push_back(item.spelling == "volatile" ? "V"
				                                             : "K");
		}
		else if (item.kind == DI_PARAMS && item.params && !clause)
			clause = item.params.get();
		else if (item.kind == DI_NESTED && item.nested && tail.empty())
			tail = WrittenOps(*item.nested);
		else
			throw OutsideBoundary("mangled dependent type form");
	}
	ApplyWrittenOps(prefix, subs, body, key);
	if (!clause)
	{
		ApplyWrittenOps(tail, subs, body, key);
		return;
	}
	string params;
	string params_key;
	for (size_t p = 0; p < clause->parameters.size(); p++)
	{
		string one_key;
		params += MangleWrittenParameter(
			clause->parameters[p],
			WrittenParameterIsPack(clause->parameters[p]), subs,
			&one_key);
		params_key += one_key + ",";
	}
	if (clause->parameters.empty())
	{
		params = "v";
		params_key = "v";
	}
	if (clause->variadic)
	{
		params += "z";
		params_key += "z";
	}
	// The typed path keys function types the same way, so written and
	// composed spellings compress against each other.
	key = "F|" + key + "|" + params_key;
	body = MangleSubstitutable(key, "F" + body + params + "E", subs);
	ApplyWrittenOps(tail, subs, body, key);
}

string MangleWrittenTypeId(const AstTypeId& id, Substitutions& subs,
                           string* key_out)
{
	string key;
	string body = MangleWrittenBase(TypeSpecifiers(id.specifiers), true,
	                                subs, key);
	if (id.declarator)
		ApplyWrittenDeclarator(*id.declarator, subs, body, key);
	if (key_out)
		*key_out = key;
	return body;
}

// A written function parameter, with the 8.3.5p5 adjustment: cv that
// ends up top-level (no enclosing pointer/reference) drops; a pack
// parameter spells `Dp<element>`.
string MangleWrittenParameter(const AstParameter& parameter, bool is_pack,
                              Substitutions& subs, string* key_out)
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
	{
		key = "Dp|" + key;
		body = MangleSubstitutable(key, "Dp" + body, subs);
	}
	if (key_out)
		*key_out = key;
	return body;
}

// The written return type: declaration specifiers plus the prefix
// pointer/reference/cv items of the pattern declarator.
string MangleWrittenReturn(const AstDecl& inner, Substitutions& subs)
{
	const AstDeclarator* declarator = WrittenDeclarator(inner);
	if (!declarator)
		throw OutsideBoundary("mangled dependent signature form");
	// An auto placeholder resolves through the trailing return
	// type-id (a written decltype spells DT..E).
	for (size_t i = 0; i < declarator->items.size(); i++)
		if (declarator->items[i].kind == DI_TRAILING_RETURN &&
		    declarator->items[i].trailing_type)
			return MangleWrittenTypeId(
				*declarator->items[i].trailing_type, subs, 0);
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
					return MangleWrittenTypeId(trailing, subs, 0);
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

string MangleTypedAliasExpansion(const TemplateInfo& alias,
                                 const vector<TemplateArg>& args,
                                 Substitutions& subs, string* key_out)
{
	if (!alias.alias_type || args.size() > alias.params.size())
		throw OutsideBoundary("mangled dependent type form");
	Substitutions::AliasFrame frame;
	frame.alias = &alias;
	frame.typed_args = &args;
	frame.outer_params = subs.pattern_params;
	frame.outer_fn_params = subs.fn_param_names;
	frame.outer_scope = subs.written_scope;
	frame.outer = subs.alias_frame;
	MangleContext saved(subs);
	subs.alias_frame = &frame;
	subs.pattern_params = 0;
	subs.fn_param_names = 0;
	subs.written_scope = TemplateLookupScope(alias);
	return MangleWrittenTypeId(*alias.alias_type, subs, key_out);
}

string MangleDependentTypeId(const AstTypeId& id, Substitutions& subs,
                             string* key_out)
{
	return MangleWrittenTypeId(id, subs, key_out);
}

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
