#include "sema/template_info.h"

#include "ast/ast_text.h"

// PA35 alias-transparent signature spelling (14.5.7): a
// specialization of an alias template is equivalent to the type-id
// obtained by substituting its arguments, so `_Require<C...>` and
// `typename enable_if<__and_<C...>::value, void>::type` declare the
// same dependent return type even though only the first composes as
// an anchor type. The canonical key renders a return-type spelling
// with alias template-ids recursively substituted, trailing
// class-template default arguments filled, and template-parameter
// names positionalized, so the redeclaration matcher can equate
// spellings the abstract composition cannot represent. A form outside
// the renderer's subset yields an empty key (no identity claim).

namespace {

struct SpellBail
{
};

// One parameter substitution: the rendered element list. A concrete
// pack (an alias parameter bound to spliced arguments) joins its
// elements at a `name...` use; an abstract pack (an unsubstituted
// declaration parameter) keeps its written ellipsis.
struct SpellSub
{
	vector<string> elements;
	bool pack = false;
	bool abstract_pack = false;
};

typedef map<string, SpellSub> SpellEnv;

enum { kSpellDepthLimit = 16 };

const ScopeBinding* FindUp(Scope* scope, const string& name)
{
	for (; scope; scope = scope->parent)
		if (const ScopeBinding* own = FindOwnBinding(*scope, name))
			return own;
	return 0;
}

bool SpellIdentChar(char c)
{
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9');
}

string JoinSpellings(const vector<string>& parts)
{
	string text;
	for (size_t i = 0; i < parts.size(); i++)
	{
		if (i)
			text += ",";
		text += parts[i];
	}
	return text;
}

// Whole-identifier substitution over flattened text (expressions and
// declarator shapes): a concrete pack's `name...` splices the joined
// element list; a bare use takes the single element.
void SubstituteText(string& text, const SpellEnv& env)
{
	for (SpellEnv::const_iterator e = env.begin(); e != env.end(); ++e)
	{
		const string& name = e->first;
		const SpellSub& sub = e->second;
		size_t at = 0;
		while ((at = text.find(name, at)) != string::npos)
		{
			bool left = at > 0 && SpellIdentChar(text[at - 1]);
			bool right = at + name.size() < text.size() &&
				SpellIdentChar(text[at + name.size()]);
			if (left || right)
			{
				at += name.size();
				continue;
			}
			size_t consume = name.size();
			string with;
			if (sub.pack && !sub.abstract_pack)
			{
				if (text.compare(at + name.size(), 3, "...") == 0)
				{
					with = JoinSpellings(sub.elements);
					consume += 3;
				}
				else if (sub.elements.size() == 1)
					with = sub.elements[0];
				else
					throw SpellBail();
			}
			else
				with = sub.elements[0];
			text.replace(at, consume, with);
			at += with.size();
		}
	}
}

string RenderTypeId(const AstTypeId& type, const SpellEnv& env,
                    Scope* scope, int depth);
string RenderName(const AstName& name, const SpellEnv& env, Scope* scope,
                  int depth);

string RenderExprText(const AstExpr& expr, const SpellEnv& env)
{
	string text = FlattenExpr(expr);
	SubstituteText(text, env);
	return text;
}

// The identifier of a bare, unqualified type-id (`C`), or null.
// `allow_pack` also accepts `C...` spelled with the ellipsis as the
// type-id's lone abstract-declarator item.
const string* BareTypeName(const AstTypeId& type, bool allow_pack = false)
{
	if (type.declarator && !type.declarator->Empty() &&
	    !(allow_pack && type.declarator->items.size() == 1 &&
	      type.declarator->items[0].kind == DI_PACK))
		return 0;
	if (type.specifiers.size() != 1 ||
	    type.specifiers[0].kind != SPEC_TYPE_NAME)
		return 0;
	const AstName& name = type.specifiers[0].name;
	if (name.global_scope || name.parts.size() != 1 ||
	    name.parts[0].kind != NP_IDENTIFIER || name.parts[0].tilde)
		return 0;
	return &name.parts[0].identifier;
}

// The pack-expansion pattern name of a `C...` template-argument
// (either the argument pack flag or a DI_PACK declarator), or null.
const string* PackExpansionName(const AstTemplateArgument& argument)
{
	if (!argument.is_type)
		return 0;
	bool declarator_pack = argument.type->declarator &&
		argument.type->declarator->items.size() == 1 &&
		argument.type->declarator->items[0].kind == DI_PACK;
	if (!argument.pack && !declarator_pack)
		return 0;
	return BareTypeName(*argument.type, true);
}

// Renders a template-argument list; a pack-expansion argument whose
// pattern is a bare parameter bound to a concrete pack splices its
// elements in place.
vector<string> RenderArgs(const vector<AstTemplateArgument>& arguments,
                          const SpellEnv& env, Scope* scope, int depth)
{
	vector<string> out;
	for (size_t i = 0; i < arguments.size(); i++)
	{
		const AstTemplateArgument& argument = arguments[i];
		string text;
		if (argument.is_type)
		{
			if (const string* bare = PackExpansionName(argument))
			{
				SpellEnv::const_iterator it = env.find(*bare);
				if (it != env.end() && it->second.pack &&
				    !it->second.abstract_pack)
				{
					for (size_t k = 0;
					     k < it->second.elements.size(); k++)
						out.push_back(it->second.elements[k]);
					continue;
				}
			}
			text = RenderTypeId(*argument.type, env, scope, depth);
		}
		else
			text = RenderExprText(*argument.expr, env);
		if (argument.pack)
			text += "...";
		out.push_back(text);
	}
	return out;
}

// Fills a class template-id's trailing defaulted arguments so
// `enable_if<X>` and `enable_if<X, void>` spell the same key. Earlier
// parameters bind for defaults that reference them; a pack, a spliced
// abstract tail, or a missing default ends the fill.
void FillDefaultArgs(const TemplateInfo& tmpl, vector<string>& args,
                     int depth)
{
	if (args.size() >= tmpl.params.size())
		return;
	for (size_t i = 0; i < args.size(); i++)
		if (args[i].size() >= 3 &&
		    args[i].compare(args[i].size() - 3, 3, "...") == 0)
			return;
	SpellEnv bound;
	for (size_t i = 0; i < tmpl.params.size(); i++)
	{
		const TemplateParam& param = tmpl.params[i];
		if (param.pack)
			return;
		if (i < args.size())
		{
			if (!param.name.empty())
			{
				SpellSub sub;
				sub.elements.push_back(args[i]);
				bound[param.name] = sub;
			}
			continue;
		}
		string filled;
		if (param.kind == TPK_TYPE && param.default_type)
			filled = RenderTypeId(*param.default_type, bound,
			                      TemplateLookupScope(tmpl), depth + 1);
		else if (param.kind == TPK_VALUE && param.default_expr)
			filled = RenderExprText(*param.default_expr, bound);
		else
			return;
		args.push_back(filled);
		if (!param.name.empty())
		{
			SpellSub sub;
			sub.elements.push_back(filled);
			bound[param.name] = sub;
		}
	}
}

// Substitutes one alias-template use: the rendered arguments (and
// rendered defaults) bind to the alias's parameter names and its
// type-id renders under the alias's own lookup scope (14.5.7).
string ExpandAlias(const TemplateInfo& tmpl, const vector<string>& args,
                   int depth)
{
	if (depth > kSpellDepthLimit)
		throw SpellBail();
	SpellEnv child;
	size_t at = 0;
	for (size_t i = 0; i < tmpl.params.size(); i++)
	{
		const TemplateParam& param = tmpl.params[i];
		SpellSub sub;
		if (param.pack)
		{
			sub.pack = true;
			while (at < args.size())
				sub.elements.push_back(args[at++]);
		}
		else if (at < args.size())
		{
			// A spliced tail of unknown arity cannot bind positionally.
			if (args[at].size() >= 3 &&
			    args[at].compare(args[at].size() - 3, 3, "...") == 0)
				throw SpellBail();
			sub.elements.push_back(args[at++]);
		}
		else if (param.kind == TPK_TYPE && param.default_type)
			sub.elements.push_back(RenderTypeId(
				*param.default_type, child, TemplateLookupScope(tmpl),
				depth + 1));
		else if (param.kind == TPK_VALUE && param.default_expr)
			sub.elements.push_back(RenderExprText(*param.default_expr,
			                                      child));
		else
			throw SpellBail();
		if (!param.name.empty())
			child[param.name] = sub;
	}
	if (at < args.size())
		throw SpellBail();
	return RenderTypeId(*tmpl.alias_type, child,
	                    TemplateLookupScope(tmpl), depth + 1);
}

// One name part; `scoped` marks a first (unqualified) part, where
// lookup can fill class-template defaults.
string RenderPart(const AstNamePart& part, const SpellEnv& env,
                  Scope* scope, int depth, bool scoped)
{
	string prefix;
	if (part.template_keyword)
		prefix += "template ";
	if (part.tilde)
		prefix += "~";
	switch (part.kind)
	{
	case NP_IDENTIFIER:
	{
		SpellEnv::const_iterator it = env.find(part.identifier);
		if (it != env.end() && !part.tilde)
		{
			const SpellSub& sub = it->second;
			if (sub.pack && !sub.abstract_pack &&
			    sub.elements.size() != 1)
				throw SpellBail();
			return prefix + sub.elements[0];
		}
		return prefix + part.identifier;
	}
	case NP_TEMPLATE_ID:
	{
		vector<string> args = RenderArgs(part.arguments, env, scope,
		                                 depth);
		if (scoped && !env.count(part.identifier))
		{
			const ScopeBinding* binding = FindUp(scope,
			                                     part.identifier);
			if (binding && binding->kind == SB_CLASS_TEMPLATE &&
			    binding->templ && binding->templ->kind != TMPL_ALIAS)
				FillDefaultArgs(*binding->templ, args, depth);
		}
		return prefix + part.identifier + "<" + JoinSpellings(args) +
			">";
	}
	default:
		throw SpellBail();
	}
}

string RenderName(const AstName& name, const SpellEnv& env, Scope* scope,
                  int depth)
{
	if (depth > kSpellDepthLimit)
		throw SpellBail();
	// An unqualified alias template-id substitutes its type-id; the
	// substituted form carries its own `typename` spelling.
	if (!name.global_scope && name.parts.size() == 1 &&
	    name.parts[0].kind == NP_TEMPLATE_ID && !name.parts[0].tilde &&
	    !env.count(name.parts[0].identifier))
	{
		const ScopeBinding* binding = FindUp(
			scope, name.parts[0].identifier);
		if (binding && binding->kind == SB_CLASS_TEMPLATE &&
		    binding->templ && binding->templ->kind == TMPL_ALIAS &&
		    binding->templ->alias_type)
			return ExpandAlias(*binding->templ,
			                   RenderArgs(name.parts[0].arguments, env,
			                              scope, depth),
			                   depth + 1);
	}
	string text;
	if (name.typename_keyword)
		text += "typename ";
	if (name.global_scope)
		text += "::";
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		if (i)
			text += "::";
		text += RenderPart(name.parts[i], env, scope, depth,
		                   i == 0 && !name.global_scope);
	}
	return text;
}

// `signature` drops the specifiers that are not part of a function
// signature (`friend`, `inline`), mirroring the raw return spelling.
string RenderSpecSeq(const AstSpecifierSeq& specifiers,
                     const SpellEnv& env, Scope* scope, int depth,
                     bool signature)
{
	string text;
	for (size_t i = 0; i < specifiers.size(); i++)
	{
		const AstSpecifier& specifier = specifiers[i];
		if (signature && specifier.kind == SPEC_KEYWORD &&
		    (specifier.keyword == KW_FRIEND ||
		     specifier.keyword == KW_INLINE))
			continue;
		string piece;
		switch (specifier.kind)
		{
		case SPEC_KEYWORD:
			piece = specifier.spelling;
			break;
		case SPEC_TYPE_NAME:
			piece = RenderName(specifier.name, env, scope, depth);
			break;
		default:
			piece = FlattenSpecifier(specifier);
			SubstituteText(piece, env);
			break;
		}
		if (!text.empty())
			text += " ";
		text += piece;
	}
	return text;
}

string RenderTypeId(const AstTypeId& type, const SpellEnv& env,
                    Scope* scope, int depth)
{
	if (depth > kSpellDepthLimit)
		throw SpellBail();
	string text = RenderSpecSeq(type.specifiers, env, scope, depth,
	                            false);
	if (type.declarator && !type.declarator->Empty())
	{
		string shape = FlattenDeclarator(*type.declarator);
		SubstituteText(shape, env);
		text += shape;
	}
	return text;
}

}  // namespace

string AliasExpandedReturnKey(const AstSpecifierSeq& specifiers,
                              const vector<TemplateParam>& params,
                              Scope* scope)
{
	SpellEnv env;
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].name.empty())
			continue;
		SpellSub sub;
		sub.elements.push_back("#" + std::to_string(i));
		sub.pack = params[i].pack;
		sub.abstract_pack = params[i].pack;
		env[params[i].name] = sub;
	}
	try
	{
		return RenderSpecSeq(specifiers, env, scope, 0, true);
	}
	catch (const SpellBail&)
	{
		return string();
	}
}
