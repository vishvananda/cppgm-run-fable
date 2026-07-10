#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA19 parameter packs: the expansion engine. A pack name binds as a
// pack alias (elements carried on the binding); every `...` expansion
// site iterates the elements, re-binding each mentioned pack name to
// its k-th element in a transient scope and re-running the ordinary
// resolution or analysis on the stored pattern - the same re-binding
// philosophy the PA18 instantiation machinery uses for whole bodies.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA19 assignment boundary");
}

// --- pack-mention collection -------------------------------------------------

struct PackMentions
{
	vector<const ScopeBinding*> packs;

	void Add(const ScopeBinding* binding)
	{
		for (size_t i = 0; i < packs.size(); i++)
			if (packs[i] == binding)
				return;
		packs.push_back(binding);
	}
};

void CollectFromName(const AstName& name, Scope* scope, PackMentions& out);
void CollectFromExpr(const AstExpr& expr, Scope* scope, PackMentions& out);
void CollectFromTypeId(const AstTypeId& type_id, Scope* scope,
                       PackMentions& out);
void CollectFromDeclarator(const AstDeclarator& declarator, Scope* scope,
                           PackMentions& out);

void CollectIdentifier(const string& identifier, Scope* scope,
                       PackMentions& out)
{
	if (identifier.empty())
		return;
	const ScopeBinding* found =
		UnqualifiedLookup(scope, identifier, SLF_ANY);
	if (found && found->is_pack)
		out.Add(found);
}

// Whether a template-argument type-id carries its own `...` marker (a
// nested expansion that consumes its packs at its own level).
static bool ArgumentTypeIdHasPackMarker(const AstTypeId& type_id)
{
	if (!type_id.declarator)
		return false;
	for (size_t i = 0; i < type_id.declarator->items.size(); i++)
		if (type_id.declarator->items[i].kind == DI_PACK)
			return true;
	return false;
}

void CollectFromName(const AstName& name, Scope* scope, PackMentions& out)
{
	for (size_t i = 0; i < name.parts.size(); i++)
	{
		const AstNamePart& part = name.parts[i];
		if (i == 0 && part.kind != NP_DECLTYPE && !name.global_scope)
			CollectIdentifier(part.identifier, scope, out);
		if (part.kind == NP_DECLTYPE && part.decltype_expr)
			CollectFromExpr(*part.decltype_expr, scope, out);
		for (size_t a = 0; a < part.arguments.size(); a++)
		{
			const AstTemplateArgument& argument = part.arguments[a];
			// 14.5.3p4: a pack mentioned only inside a nested expansion
			// is expanded there, not by the outer expansion.
			if (argument.pack ||
			    (argument.is_type && argument.type &&
			     ArgumentTypeIdHasPackMarker(*argument.type)))
				continue;
			if (argument.type)
				CollectFromTypeId(*argument.type, scope, out);
			if (argument.expr)
				CollectFromExpr(*argument.expr, scope, out);
		}
	}
}

void CollectFromSpecifiers(const AstSpecifierSeq& specifiers, Scope* scope,
                           PackMentions& out)
{
	for (size_t i = 0; i < specifiers.size(); i++)
	{
		if (specifiers[i].kind == SPEC_TYPE_NAME)
			CollectFromName(specifiers[i].name, scope, out);
		else if (specifiers[i].kind == SPEC_DECLTYPE &&
		         specifiers[i].decltype_expr)
			CollectFromExpr(*specifiers[i].decltype_expr, scope, out);
	}
}

void CollectFromDeclarator(const AstDeclarator& declarator, Scope* scope,
                           PackMentions& out)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_NESTED && item.nested)
			CollectFromDeclarator(*item.nested, scope, out);
		else if (item.kind == DI_ARRAY && item.array_bound)
			CollectFromExpr(*item.array_bound, scope, out);
		else if (item.kind == DI_PARAMS && item.params)
			for (size_t p = 0; p < item.params->parameters.size(); p++)
			{
				const AstParameter& parameter =
					item.params->parameters[p];
				CollectFromSpecifiers(parameter.specifiers, scope, out);
				if (parameter.declarator)
					CollectFromDeclarator(*parameter.declarator, scope,
					                      out);
			}
	}
}

void CollectFromTypeId(const AstTypeId& type_id, Scope* scope,
                       PackMentions& out)
{
	CollectFromSpecifiers(type_id.specifiers, scope, out);
	if (type_id.declarator)
		CollectFromDeclarator(*type_id.declarator, scope, out);
}

void CollectFromExpr(const AstExpr& expr, Scope* scope, PackMentions& out)
{
	if (expr.kind == EK_ID || expr.kind == EK_MEMBER)
		CollectFromName(expr.name, scope, out);
	if (expr.type)
		CollectFromTypeId(*expr.type, scope, out);
	for (size_t i = 0; i < expr.operands.size(); i++)
		if (expr.operands[i])
			CollectFromExpr(*expr.operands[i], scope, out);
	for (size_t i = 0; i < expr.arguments.size(); i++)
		if (expr.arguments[i])
			CollectFromExpr(*expr.arguments[i], scope, out);
}

// Whether every mentioned pack is an abstract pattern binding (its
// param_index marks the template-parameter slot; a concrete pack -
// even an empty one - has param_index -1).
bool PacksAreAbstract(const PackMentions& mentions)
{
	for (size_t i = 0; i < mentions.packs.size(); i++)
		if (mentions.packs[i]->param_index < 0)
			return false;
	return true;
}

}  // namespace

// The common element count of the mentioned packs (14.5.3p5: lockstep
// expansion requires equal lengths).
size_t SemBinder::PackExpansionLength(
	const vector<const ScopeBinding*>& packs)
{
	size_t length = 0;
	bool have = false;
	for (size_t i = 0; i < packs.size(); i++)
	{
		size_t n = packs[i]->kind == SB_PARAMETER
			? packs[i]->pack_param_names.size()
			: packs[i]->pack_args.size();
		if (!have)
		{
			length = n;
			have = true;
		}
		else if (n != length)
			throw runtime_error("pack expansion over packs of "
			                    "different lengths");
	}
	if (!have)
		throw runtime_error("pack expansion names no parameter pack");
	return length;
}

// A transient scope binding each mentioned pack name to its k-th
// element.
Scope* SemBinder::MakePackElementScope(
	const vector<const ScopeBinding*>& packs, size_t index)
{
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                  current_);
	for (size_t i = 0; i < packs.size(); i++)
	{
		const ScopeBinding& pack = *packs[i];
		ScopeBinding element;
		element.name = pack.name;
		if (pack.kind == SB_PARAMETER)
		{
			element.kind = SB_PARAMETER;
			element.type = pack.pack_args[index].type;
			element.pack_element_name = pack.pack_param_names[index];
			ScopeBinding& added = AddBinding(*scope, element);
			added.owner = pack.owner;
			added.home = pack.home;
			continue;
		}
		const TemplateArg& arg = pack.pack_args[index];
		if (!arg.is_value)
		{
			element.kind = SB_TYPE_ALIAS;
			element.type = arg.type;
		}
		else
		{
			element.kind = SB_VARIABLE;
			element.no_object = true;
			element.has_value = true;
			element.value = ConstValue(arg.value_type, arg.value_bits);
			if (arg.type)
				element.type =
					MakeCvQualifiedType(arg.type, true, false);
		}
		AddBinding(*scope, element);
	}
	return scope;
}

// Expansion of one `...` template argument (`tuple<T...>`,
// `list<integral_constant<bool, bool(Bn::value)>...>`): the pattern
// re-resolves once per element. In an abstract pattern context the
// expansion is carried unexpanded as a pack-pattern slot.
void SemBinder::ExpandTemplateArgumentPack(TemplateInfo& tmpl,
                                           const TemplateParam& param,
                                           const AstTemplateArgument& argument,
                                           vector<TemplateArg>& args,
                                           Scope*& partial)
{
	PackMentions mentions;
	if (argument.type)
		CollectFromTypeId(*argument.type, current_, mentions);
	if (argument.expr)
		CollectFromExpr(*argument.expr, current_, mentions);
	{
		// A bare expansion of a pack alias that re-binds pattern slots
		// (an alias template's pack bound to the enclosing pattern's
		// pack) forwards the slots verbatim: the expansion stays a
		// pattern over the outer parameters.
		const AstName* fwd = 0;
		if (argument.is_type && argument.type &&
		    argument.type->specifiers.size() == 1 &&
		    argument.type->specifiers[0].kind == SPEC_TYPE_NAME)
			fwd = &argument.type->specifiers[0].name;
		else if (argument.expr && argument.expr->kind == EK_ID)
			fwd = &argument.expr->name;
		if (fwd && fwd->IsPlainIdentifier())
		{
			const ScopeBinding* found = UnqualifiedLookup(
				current_, fwd->parts[0].identifier, SLF_ANY);
			if (found && found->is_pack && !found->pack_args.empty())
			{
				bool pattern_run = false;
				for (size_t i = 0; i < found->pack_args.size(); i++)
					if (TemplateArgIsDependent(found->pack_args[i]))
						pattern_run = true;
				if (pattern_run)
				{
					for (size_t i = 0; i < found->pack_args.size();
					     i++)
						args.push_back(found->pack_args[i]);
					return;
				}
			}
		}
	}
	if (mentions.packs.empty() || PacksAreAbstract(mentions))
	{
		if (!InAbstractTemplateContext())
			throw runtime_error("pack expansion names no expandable "
			                    "pack");
		TemplateArg pattern;
		pattern.pack_pattern = true;
		// A bare name re-reads as a value-parameter pack slot when it
		// binds one (`index_tuple<Indexes...>` with a value pack).
		const AstName* bare = 0;
		if (argument.is_type && argument.type &&
		    argument.type->specifiers.size() == 1 &&
		    argument.type->specifiers[0].kind == SPEC_TYPE_NAME)
			bare = &argument.type->specifiers[0].name;
		else if (argument.expr && argument.expr->kind == EK_ID)
			bare = &argument.expr->name;
		if (bare && bare->IsPlainIdentifier())
		{
			const ScopeBinding* found =
				UnqualifiedLookup(current_,
				                  bare->parts[0].identifier, SLF_ANY);
			if (found && found->kind == SB_VARIABLE &&
			    found->no_object && found->is_pack &&
			    found->param_index >= 0)
			{
				pattern.is_value = true;
				pattern.value_param = found->param_index;
				args.push_back(pattern);
				return;
			}
		}
		if (argument.is_type && argument.type)
			pattern.type = builder_.ResolveTypeId(*argument.type);
		else
			pattern.dependent_value = argument.expr.get();
		args.push_back(pattern);
		return;
	}
	size_t length = PackExpansionLength(mentions.packs);
	for (size_t k = 0; k < length; k++)
	{
		Scope* element_scope = MakePackElementScope(mentions.packs, k);
		Scope* saved = current_;
		current_ = element_scope;
		try
		{
			args.push_back(ResolveOneArgument(tmpl, param, argument,
			                                  args, partial));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
}

// Expansion of one `Base(args)...` mem-initializer (PA26 12.6.2p15):
// the initializer id and its arguments mention the same packs; each
// element re-resolves the id under the element scope. The caller
// analyzes the arguments under the returned scope.
void SemBinder::ExpandPackMemInitTargets(
	const AstMemInitializer& mem,
	std::vector<std::pair<TypePtr, Scope*>>& out)
{
	PackMentions mentions;
	CollectFromName(mem.id, current_, mentions);
	if (mem.init)
	{
		if (mem.init->kind == INIT_PAREN)
			for (size_t i = 0; i < mem.init->args.size(); i++)
				CollectFromExpr(*mem.init->args[i], current_, mentions);
		else if (mem.init->kind == INIT_BRACED && mem.init->expr)
			for (size_t i = 0;
			     i < mem.init->expr->arguments.size(); i++)
				CollectFromExpr(*mem.init->expr->arguments[i], current_,
				                mentions);
	}
	if (mentions.packs.empty() || PacksAreAbstract(mentions))
		throw runtime_error("mem-initializer pack expansion names no "
		                    "expandable pack");
	size_t length = PackExpansionLength(mentions.packs);
	for (size_t k = 0; k < length; k++)
	{
		Scope* element_scope = MakePackElementScope(mentions.packs, k);
		Scope* saved = current_;
		current_ = element_scope;
		try
		{
			out.push_back(std::make_pair(ResolveTypeName(mem.id),
			                             element_scope));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
}

// Expansion of one `Base...` base-specifier: the base name re-resolves
// once per element.
void SemBinder::ExpandPackBases(const AstBaseSpecifier& base,
                                std::vector<TypePtr>& out)
{
	PackMentions mentions;
	CollectFromName(base.name, current_, mentions);
	if (mentions.packs.empty() || PacksAreAbstract(mentions))
		throw runtime_error("base pack expansion names no expandable "
		                    "pack");
	size_t length = PackExpansionLength(mentions.packs);
	for (size_t k = 0; k < length; k++)
	{
		Scope* element_scope = MakePackElementScope(mentions.packs, k);
		Scope* saved = current_;
		current_ = element_scope;
		try
		{
			out.push_back(ResolveTypeName(base.name));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
}

// Aggregate braced initialization whose item list contains pack
// expansions: the expanded values initialize the (scalar) fields in
// order, with zero for the tail (8.5.1 over the expanded list; nested
// aggregates stay outside this slice).
void SemBinder::AppendExpandedAggregateInit(const ClassInfo& cls,
                                            const SemNode& target_proto,
                                            const AstExpr& braced,
                                            vector<SemNodePtr>& out)
{
	vector<SemValue> values;
	analyzer_.AnalyzeArgumentList(braced.arguments, values);
	size_t at = 0;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		TypePtr bare = RemoveTopCv(field.type);
		if (bare->kind == TK_CLASS || bare->kind == TK_ARRAY ||
		    IsReferenceType(field.type))
			throw OutsideBoundary("pack-expanded aggregate member "
			                      "form");
		SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
		member->name = field.name;
		member->type = field.type;
		member->category = VC_LVALUE;
		member->member_offset = field.offset;
		member->is_bit_field = field.is_bit_field;
		member->bit_offset = field.bit_offset;
		member->bit_width = field.bit_width;
		member->children.push_back(CloneSemNode(target_proto));
		if (at < values.size())
		{
			CheckListInitNarrowing(values[at], bare);
			out.push_back(MemberAssignAction(field, std::move(member),
			                                 std::move(values[at])));
			at++;
		}
		else
			out.push_back(MemberAssignAction(field, std::move(member),
			                                 ZeroValue(bare)));
	}
	if (at < values.size())
		throw runtime_error("too many braced initializers");
}

// 13.5.8: the numeric literal-operator template over the literal's
// source characters: a single `char...` value-parameter pack, one
// value argument per character.
const FunctionSpecialization* SemBinder::InstantiateCharPackLiteral(
	const ScopeBinding& binding, const string& chars)
{
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		TemplateInfo& tmpl = *binding.fn_templates[t];
		if (tmpl.params.size() != 1 || !tmpl.params[0].pack ||
		    tmpl.params[0].kind != TPK_VALUE)
			continue;
		TypePtr param_type;
		try
		{
			param_type = ValueParamType(tmpl.params[0], 0);
		}
		catch (const std::exception&)
		{
			continue;
		}
		if (!IsIntegralType(param_type) ||
		    param_type->fundamental != FT_CHAR)
			continue;
		vector<TemplateArg> args;
		for (size_t i = 0; i < chars.size(); i++)
		{
			TemplateArg arg;
			arg.is_value = true;
			arg.type = param_type;
			arg.value_type = FT_CHAR;
			arg.value_bits = (unsigned long long)(unsigned char)chars[i];
			args.push_back(arg);
		}
		return EnsureFunctionSpecialization(tmpl, args);
	}
	return 0;
}

// IConstExprContext: sizeof...(name) inside constant expressions
// (false when the name binds no concrete pack, so abstract patterns
// stay dependent).
bool SemBinder::PackSizeConstant(const string& name,
                                 unsigned long long& out)
{
	const ScopeBinding* found = UnqualifiedLookup(current_, name,
	                                              SLF_ANY);
	if (!found || !found->is_pack || found->param_index >= 0)
		return false;
	out = found->kind == SB_PARAMETER ? found->pack_param_names.size()
	                                  : found->pack_args.size();
	return true;
}

// The element count behind `sizeof...(name)`.
size_t SemBinder::PackSize(const string& name)
{
	const ScopeBinding* found = UnqualifiedLookup(current_, name,
	                                              SLF_ANY);
	if (!found || !found->is_pack)
		throw runtime_error(name + " does not name a parameter pack");
	return found->kind == SB_PARAMETER ? found->pack_param_names.size()
	                                   : found->pack_args.size();
}

// Expansion of one `pattern...` expression inside an argument or
// initializer list: the pattern re-analyzes once per element.
bool SemBinder::ExpandPackExpression(const AstExpr& pattern,
                                     vector<SemValue>& out)
{
	PackMentions mentions;
	CollectFromExpr(pattern, current_, mentions);
	if (mentions.packs.empty())
		return false;
	// Abstract pattern bindings carry no elements; expanding them
	// here would silently produce zero values instead of deferring.
	if (PacksAreAbstract(mentions))
		throw runtime_error("pack expansion in a non-instantiated "
		                    "template context");
	size_t length = PackExpansionLength(mentions.packs);
	for (size_t k = 0; k < length; k++)
	{
		Scope* element_scope = MakePackElementScope(mentions.packs, k);
		Scope* saved = current_;
		current_ = element_scope;
		try
		{
			out.push_back(analyzer_.Analyze(pattern));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	return true;
}

// Expansion of one pack-expanded function parameter (`Args... args`)
// during signature composition: one composed parameter per element,
// named per the reference convention (the declared name, then
// `name__pack2`, `name__pack3`, ...). False in abstract pattern
// contexts (the caller keeps the unexpanded pattern).
bool SemBinder::ExpandPackParameter(const AstParameter& parameter,
                                    vector<ParameterInfo>& out)
{
	PackMentions mentions;
	CollectFromSpecifiers(parameter.specifiers, current_, mentions);
	if (parameter.declarator)
		CollectFromDeclarator(*parameter.declarator, current_, mentions);
	if (mentions.packs.empty() || PacksAreAbstract(mentions))
		return false;
	size_t length = PackExpansionLength(mentions.packs);
	string declared;
	if (parameter.declarator)
		if (const AstName* id = parameter.declarator->IdName())
			if (id->IsPlainIdentifier())
				declared = id->parts[0].identifier;
	last_pack_param_ = PackParamRecord();
	last_pack_param_.name = declared;
	for (size_t k = 0; k < length; k++)
	{
		Scope* element_scope = MakePackElementScope(mentions.packs, k);
		Scope* saved = current_;
		current_ = element_scope;
		try
		{
			DeclSpecifierInfo specs =
				builder_.ProcessSpecifiers(parameter.specifiers, false);
			DeclaratorInfo composed = builder_.ComposeDeclarator(
				parameter.declarator.get(), specs.type);
			ParameterInfo info;
			info.type = composed.type;
			if (!declared.empty())
				info.name = k == 0
					? declared
					: declared + "__pack" + std::to_string(k + 1);
			out.push_back(info);
			last_pack_param_.names.push_back(info.name);
			last_pack_param_.types.push_back(
				AdjustParameterType(composed.type));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	// 8.3.5p2: a trailing-return decltype composes right after the
	// clause and must already see this pack (`decltype(f(args...))`);
	// publish the captured binding now (the caller's post-composition
	// publish then finds the record consumed and keeps the binding).
	BindCapturedPackParameter(param_capture_scope_);
	return true;
}

// PA21: inside an abstract pattern (partial-specialization or
// function-template signature composition), a pack-expanded parameter
// composes as its unexpanded element pattern; the caller marks the
// function-type entry pack_expansion so deduction absorbs a run.
bool SemBinder::ComposeAbstractPackParameter(const AstParameter& parameter,
                                             ParameterInfo& out)
{
	PackMentions mentions;
	CollectFromSpecifiers(parameter.specifiers, current_, mentions);
	if (parameter.declarator)
		CollectFromDeclarator(*parameter.declarator, current_, mentions);
	if (mentions.packs.empty() || !PacksAreAbstract(mentions))
		return false;
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(parameter.specifiers, false);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		parameter.declarator.get(), specs.type);
	out.type = composed.type;
	out.pack_pattern = true;
	if (composed.id && composed.id->IsPlainIdentifier())
		out.name = composed.id->parts[0].identifier;
	return true;
}

// After a signature composed with an expanded pack parameter, the
// function scope gains (or completes) the pack binding: `args...`
// expansions and sizeof...(args) inside the body read it.
void SemBinder::BindCapturedPackParameter(Scope* scope)
{
	if (!scope || last_pack_param_.name.empty())
		return;
	ScopeBinding* binding = FindOwnBinding(*scope,
	                                       last_pack_param_.name);
	if (!binding)
	{
		ScopeBinding fresh;
		fresh.kind = SB_PARAMETER;
		fresh.name = last_pack_param_.name;
		binding = &AddBinding(*scope, fresh);
	}
	binding->is_pack = true;
	binding->pack_param_names = last_pack_param_.names;
	binding->pack_args.clear();
	for (size_t i = 0; i < last_pack_param_.types.size(); i++)
		binding->pack_args.push_back(
			TemplateArg(last_pack_param_.types[i]));
	last_pack_param_ = PackParamRecord();
}
