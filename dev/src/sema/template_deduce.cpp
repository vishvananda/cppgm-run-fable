#include "sema/sem_binder.h"

#include <stdexcept>

#include "ast/ast_text.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA18 function templates: deduction patterns, direct-call argument
// deduction (14.8.2.1 subset), and on-demand instantiation of
// function-template specializations. Patterns compose over shared
// positional placeholder types (`#0`, `#1`, ...) so redeclarations
// with renamed parameters compare structurally; substitution composes
// the declarator again with the parameters aliased to the concrete
// types, so dependent forms (`typename T::type`) resolve through the
// ordinary machinery.

namespace {

// Removes exactly the cv-qualifiers the pattern spelled from the
// deduced type (`const T&` binding `const X` deduces `T = X`).
TypePtr StripPatternCv(const TypePtr& pattern, const TypePtr& deduced)
{
	if ((!pattern->is_const && !pattern->is_volatile) ||
	    IsReferenceType(deduced))
		return deduced;
	Type stripped = *deduced;
	if (pattern->is_const)
		stripped.is_const = false;
	if (pattern->is_volatile)
		stripped.is_volatile = false;
	return TypePtr(new Type(stripped));
}

// Whether a deduction slot has been bound (a type, a value, a
// template, or a completed pack run).
bool ArgBound(const TemplateArg& arg)
{
	if (arg.is_pack_slot)
		return arg.pack_done;
	return arg.is_value || bool(arg.type) || arg.template_entity;
}

bool DeduceFromType(const TypePtr& pattern, const TypePtr& arg,
                    vector<TemplateArg>& bound, bool exact_cv = true);
bool DeduceFromArgList(const vector<TemplateArg>& pattern,
                       const vector<TemplateArg>& args,
                       vector<TemplateArg>& bound, bool allow_trailing);

// Unification of one template-argument slot of a specialization
// pattern against the corresponding concrete argument. Value slots
// bind through the pattern's value-parameter reference or compare by
// value; template-template slots bind by template identity; type
// slots recurse.
bool DeduceFromArg(const TemplateArg& pattern, const TemplateArg& arg,
                   vector<TemplateArg>& bound)
{
	// PA21 template-template slots: a pattern slot forwarding a
	// template-template parameter binds the argument's template; a
	// concrete template in the pattern must match exactly.
	if (pattern.template_param >= 0)
	{
		if (!arg.template_entity ||
		    (size_t)pattern.template_param >= bound.size())
			return false;
		TemplateArg& slot = bound[pattern.template_param];
		if (ArgBound(slot))
			return slot.template_entity == arg.template_entity;
		TemplateArg fresh;
		fresh.template_entity = arg.template_entity;
		fresh.is_pack_slot = slot.is_pack_slot;
		bound[pattern.template_param] = fresh;
		return true;
	}
	if (pattern.template_entity || arg.template_entity)
		return pattern.template_entity == arg.template_entity;
	// A deferred type slot re-resolves after structural deduction
	// (CheckDependentPatternSlots); it matches anything here. A
	// dependent alias-template use (void_t<typename T::x>) is the
	// same: 14.5.7p2 matching re-substitutes rather than comparing
	// the alias node structurally.
	if (pattern.dependent_type)
		return true;
	if (!pattern.is_value && TypeIsDependentAliasUse(pattern.type))
		return true;
	if (!pattern.is_value && !arg.is_value)
		return DeduceFromType(pattern.type, arg.type, bound);
	if (!pattern.is_value || !arg.is_value)
		return false;
	if (pattern.value_param >= 0)
	{
		if ((size_t)pattern.value_param >= bound.size())
			return false;
		TemplateArg& slot = bound[pattern.value_param];
		if (ArgBound(slot))
			return TemplateArgEquals(slot, arg);
		bool keep_pack = slot.is_pack_slot;
		slot = arg;
		slot.is_pack_slot = keep_pack;
		return true;
	}
	// A concrete value in the pattern must match exactly.
	return TemplateArgEquals(pattern, arg);
}

// The pack slots a pattern type mentions (TK_TYPE_PARAM placeholders
// whose bound slot is a pack).
void CollectPatternPackSlots(const TypePtr& pattern,
                             const vector<TemplateArg>& bound,
                             std::vector<size_t>& out)
{
	if (!pattern)
		return;
	if (pattern->kind == TK_TYPE_PARAM && pattern->named->param_index >= 0)
	{
		size_t index = (size_t)pattern->named->param_index;
		if (index < bound.size() && bound[index].is_pack_slot)
		{
			for (size_t i = 0; i < out.size(); i++)
				if (out[i] == index)
					return;
			out.push_back(index);
		}
		return;
	}
	for (size_t i = 0; i < pattern->parameters.size(); i++)
		CollectPatternPackSlots(pattern->parameters[i], bound, out);
	for (size_t i = 0; i < pattern->targs.size(); i++)
		if (!pattern->targs[i].is_value)
			CollectPatternPackSlots(pattern->targs[i].type, bound, out);
	CollectPatternPackSlots(pattern->target, bound, out);
}

// 14.8.2.5p9 (subset): one `pattern...` slot of a template-id pattern
// absorbs the argument run [begin, end); each element unifies against
// the element pattern with the mentioned pack slots accumulating one
// element per iteration. Repeated expansions of one pack must deduce
// the same run.
bool DeducePackPattern(const TemplateArg& pattern,
                       const vector<TemplateArg>& args, size_t begin,
                       size_t end, vector<TemplateArg>& bound)
{
	// The pack slots this pattern element mentions (accumulation
	// targets). A direct slot reference is the common fast path.
	std::vector<size_t> slots;
	int direct = -1;
	if (pattern.is_value && pattern.value_param >= 0)
	{
		if ((size_t)pattern.value_param >= bound.size() ||
		    !bound[pattern.value_param].is_pack_slot)
			return false;
		direct = pattern.value_param;
		slots.push_back((size_t)direct);
	}
	else if (!pattern.is_value && pattern.type &&
	         pattern.type->kind == TK_TYPE_PARAM &&
	         pattern.type->named->param_index >= 0 &&
	         (size_t)pattern.type->named->param_index < bound.size() &&
	         bound[pattern.type->named->param_index].is_pack_slot)
	{
		direct = pattern.type->named->param_index;
		slots.push_back((size_t)direct);
	}
	else if (!pattern.is_value)
	{
		CollectPatternPackSlots(pattern.type, bound, slots);
		if (slots.empty())
			return false;
	}
	else
		return false;
	vector<vector<TemplateArg>> runs(slots.size());
	for (size_t ai = begin; ai < end; ai++)
	{
		const TemplateArg& arg = args[ai];
		if (direct >= 0)
		{
			// The element binds the pack slot wholesale (type or value
			// element alike, matching the pattern's kind).
			if (pattern.is_value != arg.is_value)
				return false;
			TemplateArg element = arg;
			element.pack_pattern = false;
			runs[0].push_back(element);
			continue;
		}
		// Composite element pattern: unify against a probe with the
		// mentioned pack slots cleared, then collect their elements.
		vector<TemplateArg> probe = bound;
		for (size_t s = 0; s < slots.size(); s++)
		{
			probe[slots[s]] = TemplateArg();
			probe[slots[s]].is_pack_slot = false;
		}
		TemplateArg element_pattern = pattern;
		element_pattern.pack_pattern = false;
		if (!DeduceFromArg(element_pattern, arg, probe))
			return false;
		for (size_t s = 0; s < slots.size(); s++)
		{
			if (!(probe[slots[s]].is_value || probe[slots[s]].type ||
			      probe[slots[s]].template_entity))
				return false;
			runs[s].push_back(probe[slots[s]]);
		}
		// Fixed slots newly bound by the probe propagate.
		for (size_t j = 0; j < probe.size(); j++)
			if (!bound[j].is_pack_slot && !ArgBound(bound[j]) &&
			    ArgBound(probe[j]))
				bound[j] = probe[j];
	}
	for (size_t s = 0; s < slots.size(); s++)
	{
		TemplateArg& slot = bound[slots[s]];
		if (slot.pack_done)
		{
			if (slot.pack_elements.size() != runs[s].size())
				return false;
			for (size_t k = 0; k < runs[s].size(); k++)
				if (!TemplateArgEquals(slot.pack_elements[k],
				                       runs[s][k]))
					return false;
			continue;
		}
		slot.pack_done = true;
		slot.pack_elements = runs[s];
	}
	return true;
}

// Unification of a template-id pattern argument list against a
// resolved argument list: fixed slots one-to-one, a `pattern...` slot
// absorbing the run the fixed tail leaves. `allow_trailing` accepts
// concrete arguments beyond the pattern (a defaulted tail).
bool DeduceFromArgList(const vector<TemplateArg>& pattern,
                       const vector<TemplateArg>& args,
                       vector<TemplateArg>& bound, bool allow_trailing)
{
	size_t ai = 0;
	for (size_t pi = 0; pi < pattern.size(); pi++)
	{
		const TemplateArg& p = pattern[pi];
		if (p.pack_pattern)
		{
			size_t fixed_tail = pattern.size() - pi - 1;
			if (args.size() < ai + fixed_tail)
				return false;
			size_t end = args.size() - fixed_tail;
			if (!DeducePackPattern(p, args, ai, end, bound))
				return false;
			ai = end;
			continue;
		}
		if (ai >= args.size())
			return false;
		// Ordering: a fixed slot never matches a pack expansion (the
		// fixed list is more specialized, 14.8.2.4p11).
		if (args[ai].pack_pattern)
			return false;
		if (!DeduceFromArg(p, args[ai], bound))
			return false;
		ai++;
	}
	return allow_trailing || ai == args.size();
}

// 14.8.2.5: structural unification of one parameter pattern against
// one argument type. `bound` has one slot per template parameter.
// `exact_cv` enforces the deduced-A-equals-A identity (partial
// specializations, ordering); call-argument deduction relaxes it at
// the top level of each parameter (14.8.2.1p3).
bool DeduceFromType(const TypePtr& pattern, const TypePtr& arg,
                    vector<TemplateArg>& bound, bool exact_cv)
{
	if (!pattern || !arg)
		return false;
	if (pattern->kind == TK_TYPE_PARAM)
	{
		int index = pattern->named->param_index;
		if (index < 0 || (size_t)index >= bound.size())
			return false;
		// 14.8.2.5: a cv-qualified pattern (`const T`) only deduces
		// from an argument carrying those qualifiers (the deduced A
		// must be identical to A).
		if (exact_cv &&
		    ((pattern->is_const && !arg->is_const) ||
		     (pattern->is_volatile && !arg->is_volatile)))
			return false;
		TypePtr deduced = StripPatternCv(pattern, arg);
		if (ArgBound(bound[index]))
			return bound[index].type &&
				TypeEquals(bound[index].type, deduced);
		bound[index] = TemplateArg(deduced);
		return true;
	}
	if (pattern->kind != arg->kind)
	{
		// 14.8.2.1p4: a class-template parameter can deduce from a
		// derived class's unique matching base.
		if (pattern->kind == TK_TEMPLATE_SPEC && arg->kind == TK_CLASS)
			;  // handled below
		else
			return false;
	}
	switch (pattern->kind)
	{
	case TK_FUNDAMENTAL:
		return pattern->fundamental == arg->fundamental &&
			pattern->is_const == arg->is_const &&
			pattern->is_volatile == arg->is_volatile;
	case TK_CLASS:
	case TK_ENUM:
		return pattern->named == arg->named &&
			pattern->is_const == arg->is_const &&
			pattern->is_volatile == arg->is_volatile;
	case TK_POINTER:
		if (pattern->is_const != arg->is_const ||
		    pattern->is_volatile != arg->is_volatile)
			return false;
		// 14.8.2.1p4: call deduction lets the deduced pointee be less
		// cv-qualified than the pattern spells one level down.
		return DeduceFromType(pattern->target, arg->target, bound,
		                      exact_cv);
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_ARRAY:
		// PA21 `T[N]` pattern: the bound binds its value slot.
		if (pattern->bound_param >= 0)
		{
			if (!arg->bound_known ||
			    (size_t)pattern->bound_param >= bound.size())
				return false;
			TemplateArg& slot = bound[pattern->bound_param];
			if (ArgBound(slot))
			{
				if (!slot.is_value || slot.value_bits != arg->bound)
					return false;
			}
			else
			{
				bool keep_pack = slot.is_pack_slot;
				slot = TemplateArg();
				slot.is_value = true;
				slot.type =
					MakeFundamentalType(FT_UNSIGNED_LONG_INT);
				slot.value_type = FT_UNSIGNED_LONG_INT;
				slot.value_bits = arg->bound;
				slot.is_pack_slot = keep_pack;
			}
			return DeduceFromType(pattern->target, arg->target, bound);
		}
		if (pattern->bound_known &&
		    (!arg->bound_known || pattern->bound != arg->bound))
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_FUNCTION:
	{
		// Function-type patterns compare cv/ref-qualifiers (the method
		// forms `R(A...) const` are distinct types) and absorb
		// `P...` parameter runs structurally.
		if (pattern->variadic != arg->variadic ||
		    pattern->ref_qual != arg->ref_qual ||
		    pattern->is_const != arg->is_const ||
		    pattern->is_volatile != arg->is_volatile)
			return false;
		vector<TemplateArg> pattern_params;
		for (size_t i = 0; i < pattern->parameters.size(); i++)
		{
			TemplateArg entry(pattern->parameters[i]);
			entry.pack_pattern = pattern->parameters[i]->pack_expansion;
			pattern_params.push_back(entry);
		}
		vector<TemplateArg> arg_params;
		for (size_t i = 0; i < arg->parameters.size(); i++)
		{
			TemplateArg entry(arg->parameters[i]);
			entry.pack_pattern = arg->parameters[i]->pack_expansion;
			arg_params.push_back(entry);
		}
		if (!DeduceFromArgList(pattern_params, arg_params, bound,
		                       false))
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	}
	case TK_MEMBER_POINTER:
		if (pattern->named != arg->named)
			return false;
		return DeduceFromType(pattern->target, arg->target, bound);
	case TK_TEMPLATE_SPEC:
	{
		// The deduced A must carry the pattern's exact qualification
		// (a top-level cv wrapper pattern owns cv-qualified
		// arguments); call deduction only needs the pattern to cover
		// the argument's qualification.
		if (pattern->is_const != arg->is_const ||
		    pattern->is_volatile != arg->is_volatile)
		{
			if (exact_cv)
				return false;
			if ((arg->is_const && !pattern->is_const) ||
			    (arg->is_volatile && !pattern->is_volatile))
				return false;
		}
		// PA21: a pattern anchored on a template-template parameter
		// placeholder binds the argument's template into its slot, then
		// unifies the argument lists.
		int tt_slot = pattern->named->is_template_anchor &&
			pattern->named->param_index >= 0
			? pattern->named->param_index : -1;
		// A pattern-shaped argument (partial ordering's transformed
		// parameter types) matches the same template structurally.
		if (arg->kind == TK_TEMPLATE_SPEC)
		{
			if (tt_slot >= 0)
			{
				TemplateArg as_template;
				if (arg->named->spec_template)
					as_template.template_entity =
						arg->named->spec_template;
				TemplateArg slot_pattern;
				slot_pattern.template_param = tt_slot;
				if (!DeduceFromArg(slot_pattern, as_template, bound))
					return false;
			}
			else if (arg->named->spec_template !=
			         pattern->named->spec_template)
				return false;
			return DeduceFromArgList(pattern->targs, arg->targs, bound,
			                         false);
		}
		// Match a specialization of the same template, walking the
		// single-inheritance chain for the derived-to-base case.
		const NamedTypeInfo* entity =
			arg->kind == TK_CLASS ? arg->named : 0;
		if (tt_slot >= 0)
		{
			if (!entity || !entity->spec_template)
				return false;
			TemplateArg as_template;
			as_template.template_entity = entity->spec_template;
			TemplateArg slot_pattern;
			slot_pattern.template_param = tt_slot;
			if (!DeduceFromArg(slot_pattern, as_template, bound))
				return false;
			return DeduceFromArgList(pattern->targs, entity->spec_args,
			                         bound, false);
		}
		// 14.8.2.1p4: the (unique) base from which deduction succeeds;
		// a same-template base whose arguments do not unify keeps the
		// walk going (impl<I+1, Tail...> recursion chains).
		while (entity)
		{
			if (entity->spec_template == pattern->named->spec_template)
			{
				vector<TemplateArg> probe = bound;
				if (DeduceFromArgList(pattern->targs, entity->spec_args,
				                      probe, false))
				{
					bound.swap(probe);
					return true;
				}
			}
			entity = entity->base_entity;
		}
		return false;
	}
	default:
		return false;
	}
}

// The declared argument type of one call argument for deduction
// purposes (14.8.2.1p2): arrays and functions decay, top-level cv
// drops for by-value parameters.
TypePtr DecayForDeduction(const TypePtr& type)
{
	return AdjustParameterType(type);
}

// The template-parameter slots a parameter pattern mentions (type
// parameters, array-bound value slots, template-template anchors,
// forwarded value/template slots). False when the pattern carries a
// form whose slots cannot be enumerated (deferred dependent
// type-ids), so callers stay conservative.
bool CollectPatternSlots(const TypePtr& pattern, std::vector<size_t>& out)
{
	if (!pattern)
		return true;
	if (pattern->kind == TK_TYPE_PARAM)
	{
		if (pattern->named->param_index >= 0)
			out.push_back((size_t)pattern->named->param_index);
		return true;
	}
	if (pattern->kind == TK_ARRAY && pattern->bound_param >= 0)
		out.push_back((size_t)pattern->bound_param);
	if (pattern->kind == TK_TEMPLATE_SPEC)
	{
		if (pattern->named->is_template_anchor &&
		    pattern->named->param_index >= 0)
			out.push_back((size_t)pattern->named->param_index);
		for (size_t i = 0; i < pattern->targs.size(); i++)
		{
			const TemplateArg& arg = pattern->targs[i];
			if (arg.dependent_type || arg.dependent_value)
				return false;
			if (arg.value_param >= 0)
				out.push_back((size_t)arg.value_param);
			if (arg.template_param >= 0)
				out.push_back((size_t)arg.template_param);
			if (!arg.is_value && arg.type &&
			    !CollectPatternSlots(arg.type, out))
				return false;
		}
	}
	for (size_t i = 0; i < pattern->parameters.size(); i++)
		if (!CollectPatternSlots(pattern->parameters[i], out))
			return false;
	return !pattern->target || CollectPatternSlots(pattern->target, out);
}

// Whether an explicit template argument spells a pack expansion
// (`Args...`): the argument's own flag, or the type-id's abstract
// declarator carrying the `...` as a DI_PACK item.
bool ExplicitArgumentIsExpansion(const AstTemplateArgument& argument)
{
	if (argument.pack)
		return true;
	if (!argument.is_type || !argument.type || !argument.type->declarator)
		return false;
	for (size_t i = 0; i < argument.type->declarator->items.size(); i++)
		if (argument.type->declarator->items[i].kind == DI_PACK)
			return true;
	return false;
}

// The bare single-identifier name of an expansion's pattern type-id
// (`Args...`), or null for composite patterns.
const AstName* ExpansionPackName(const AstTemplateArgument& argument)
{
	if (!argument.is_type || !argument.type)
		return 0;
	if (argument.type->specifiers.size() != 1 ||
	    argument.type->specifiers[0].kind != SPEC_TYPE_NAME)
		return 0;
	const AstName& name = argument.type->specifiers[0].name;
	if (name.parts.size() != 1 ||
	    name.parts[0].kind != NP_IDENTIFIER || name.parts[0].tilde)
		return 0;
	return &name;
}

// PA21 constructor templates: the pattern declaration is a special
// member (no decl-specifier-seq; the composed base type is void).
bool PatternIsSpecialMember(const AstDecl& inner)
{
	return inner.kind == DK_SPECIAL_MEMBER_DEFINITION ||
		inner.kind == DK_SPECIAL_MEMBER_DECLARATION;
}

// The declarator of a function-template pattern declaration.
const AstDeclarator* PatternDeclarator(const AstDecl& inner)
{
	if (inner.kind == DK_SIMPLE)
		return inner.declarators.empty()
			? 0 : inner.declarators[0].declarator.get();
	return inner.declarator.get();
}

// The parameter clause of a function declarator (the first DI_PARAMS
// item, through one nesting level).
const AstParameterClause* FunctionParameterClause(
	const AstDeclarator& declarator)
{
	for (size_t i = 0; i < declarator.items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator.items[i];
		if (item.kind == DI_PARAMS)
			return item.params.get();
		if (item.kind == DI_NESTED && item.nested)
			if (const AstParameterClause* inner =
			        FunctionParameterClause(*item.nested))
				return inner;
	}
	return 0;
}

}  // namespace

TypePtr SemBinder::PlaceholderType(size_t index)
{
	while (placeholders_.size() <= index)
	{
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			"typename #" + std::to_string(placeholders_.size()),
			model_.global(), "#" + std::to_string(placeholders_.size()));
		info->param_index = (int)placeholders_.size();
		placeholders_.push_back(MakeNamedType(TK_TYPE_PARAM, info));
	}
	return placeholders_[index];
}


// Composes the declarator of a function-template declaration with the
// parameters bound to the positional placeholders. Returns false when
// the signature does not compose abstractly (dependent qualified
// forms); per-parameter patterns then compose individually.
// The abstract pattern scope: each named parameter binds its
// positional placeholder (type placeholders, template-template
// anchors, objectless value slots); packs stay unexpanded through
// the param_index marker.
Scope* SemBinder::MakePatternParamScope(const vector<TemplateParam>& params,
                                        Scope* declaring)
{
	Scope* scope = model_.CreateScope(SCOPE_TEMPLATE_PARAMS, "",
	                                  declaring);
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].name.empty())
			continue;
		ScopeBinding binding;
		binding.name = params[i].name;
		if (params[i].kind == TPK_TYPE)
		{
			binding.kind = SB_TYPE;
			binding.type = PlaceholderType(i);
		}
		else if (params[i].kind == TPK_TEMPLATE)
		{
			// PA21: the parameter binds a placeholder template whose
			// anchor carries the slot for deduction.
			binding.kind = SB_CLASS_TEMPLATE;
			binding.templ = TemplateParamPlaceholder(params[i], i);
		}
		else
		{
			// An abstract value parameter: uses of the name inside the
			// pattern signature become value-parameter slots.
			binding.kind = SB_VARIABLE;
			binding.no_object = true;
		}
		binding.param_index = (int)i;
		binding.is_pack = params[i].pack;
		AddBinding(*scope, binding);
	}
	return scope;
}

bool SemBinder::ComposeFunctionPattern(
	const vector<TemplateParam>& params, Scope* declaring,
	const AstDecl& inner, TypePtr& full, vector<TypePtr>& param_patterns,
	vector<bool>& pattern_packs)
{
	Scope* scope = MakePatternParamScope(params, declaring);
	const AstDeclarator* declarator = PatternDeclarator(inner);
	Scope* saved = current_;
	current_ = scope;
	bool composed_full = false;
	full = TypePtr();
	param_patterns.clear();
	pattern_packs.clear();
	try
	{
		DeclSpecifierInfo specs;
		if (PatternIsSpecialMember(inner))
			specs.type = MakeFundamentalType(FT_VOID);
		else
			specs = builder_.ProcessSpecifiers(inner.specifiers, true);
		DeclaratorInfo composed =
			builder_.ComposeDeclarator(declarator, specs.type);
		if (composed.declares_function &&
		    composed.type->kind == TK_FUNCTION)
		{
			full = composed.type;
			// PA21: pack-expanded parameters compose as unexpanded
			// element patterns; the per-parameter pack flags keep the
			// deduction loop's run-absorption behavior.
			for (size_t i = 0; i < composed.parameters.size(); i++)
			{
				param_patterns.push_back(composed.parameters[i].type);
				pattern_packs.push_back(
					composed.parameters[i].pack_pattern);
			}
			composed_full = true;
		}
	}
	catch (const std::exception&)
	{
		// Dependent forms in the signature: fall through to the
		// per-parameter pass.
	}
	if (!composed_full && declarator)
	{
		// Compose each parameter's declared type individually; a
		// parameter that still fails is a non-deduced context. A
		// pack-expanded parameter's pattern is its element pattern.
		const AstParameterClause* clause = FunctionParameterClause(
			*declarator);
		if (clause)
			for (size_t i = 0; i < clause->parameters.size(); i++)
			{
				const AstParameter& parameter = clause->parameters[i];
				bool is_pack = false;
				if (parameter.declarator)
					for (size_t d = 0;
					     d < parameter.declarator->items.size(); d++)
						if (parameter.declarator->items[d].kind ==
						    DI_PACK)
							is_pack = true;
				TypePtr pattern;
				try
				{
					DeclSpecifierInfo pspecs = builder_.ProcessSpecifiers(
						parameter.specifiers, false);
					DeclaratorInfo pcomposed = builder_.ComposeDeclarator(
						parameter.declarator.get(), pspecs.type);
					pattern = pcomposed.type;
				}
				catch (const std::exception&)
				{
					pattern = TypePtr();
				}
				param_patterns.push_back(pattern);
				pattern_packs.push_back(is_pack);
			}
	}
	current_ = saved;
	return composed_full;
}

void SemBinder::EnsureFunctionPattern(TemplateInfo& tmpl)
{
	if (tmpl.pattern_ready)
		return;
	tmpl.pattern_ready = true;
	tmpl.return_pattern = TypePtr();
	ComposeFunctionPattern(tmpl.params, TemplateLookupScope(tmpl),
	                       *tmpl.pattern_decl, tmpl.pattern,
	                       tmpl.param_patterns,
	                       tmpl.param_pattern_packs);
	if (!tmpl.pattern)
		tmpl.return_pattern = ComposeReturnPattern(tmpl);
}

TypePtr SemBinder::ComposeReturnPattern(TemplateInfo& tmpl)
{
	if (!tmpl.pattern_decl || PatternIsSpecialMember(*tmpl.pattern_decl))
		return TypePtr();
	const AstDeclarator* declarator = PatternDeclarator(*tmpl.pattern_decl);
	if (!declarator)
		return TypePtr();
	// The declared return type is the specifier base wrapped by the
	// prefix pointer/reference/cv items; any other prefix form
	// (nested declarators, trailing returns) stays out of the subset.
	AstDeclarator prefix;
	for (size_t i = 0; i < declarator->items.size(); i++)
	{
		const AstDeclaratorItem& item = declarator->items[i];
		if (item.kind == DI_ID || item.kind == DI_PARAMS)
			break;
		if (item.kind != DI_PTR && item.kind != DI_CV)
			return TypePtr();
		prefix.items.emplace_back();
		prefix.items.back().kind = item.kind;
		prefix.items.back().token = item.token;
		prefix.items.back().spelling = item.spelling;
	}
	Scope* scope = MakePatternParamScope(tmpl.params,
	                                     TemplateLookupScope(tmpl));
	Scope* saved = current_;
	current_ = scope;
	TypePtr result;
	try
	{
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(tmpl.pattern_decl->specifiers, true);
		result = builder_.ComposeDeclarator(&prefix, specs.type).type;
	}
	catch (const std::exception&)
	{
		// A dependent return type keeps its written form; the mangler
		// walks the AST.
	}
	current_ = saved;
	return result;
}

bool SemBinder::SameFunctionTemplateSignature(TemplateInfo& tmpl,
                                              const AstDecl& decl,
                                              const AstDecl& inner,
                                              bool match_head_spelling)
{
	EnsureFunctionPattern(tmpl);
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
	if (!SameTemplateParameterKinds(params, tmpl.params))
		return false;
	// 14.5.6.1p6: differing template heads (non-type parameter types
	// included) declare distinct templates that overload. An
	// out-of-class definition pairing skips the spelling comparison:
	// its head may spell the same type through the class's own
	// typedefs (`value_type` vs `T`), and pairing tolerance is safe
	// because the declaration it completes already exists.
	if (match_head_spelling &&
	    !SameTemplateParameterLists(params, tmpl.params))
		return false;
	TypePtr full;
	vector<TypePtr> param_patterns;
	vector<bool> pattern_packs;
	bool composed = ComposeFunctionPattern(params,
	                                       TemplateLookupScope(tmpl), inner,
	                                       full, param_patterns,
	                                       pattern_packs);
	if (composed != bool(tmpl.pattern))
		return false;
	if (composed)
		return TypeEquals(full, tmpl.pattern);
	// Neither signature composes abstractly: compare the composable
	// parameter patterns positionally as a conservative identity.
	if (param_patterns.size() != tmpl.param_patterns.size())
		return false;
	for (size_t i = 0; i < param_patterns.size(); i++)
	{
		if (bool(param_patterns[i]) != bool(tmpl.param_patterns[i]))
			return false;
		if (param_patterns[i] &&
		    !TypeEquals(param_patterns[i], tmpl.param_patterns[i]))
			return false;
	}
	// The dependent parameter spelling distinguishes overloads whose
	// non-composing patterns compare equal as nulls
	// (`remove_reference<T>::type&` vs `...type&&`).
	const AstDeclarator* decl_declarator = PatternDeclarator(inner);
	const AstDeclarator* tmpl_declarator =
		PatternDeclarator(*tmpl.pattern_decl);
	if (bool(decl_declarator) != bool(tmpl_declarator))
		return false;
	if (decl_declarator &&
	    CanonicalDeclaratorParams(*decl_declarator, params) !=
	        CanonicalDeclaratorParams(*tmpl_declarator, tmpl.params))
		return false;
	// The dependent return spelling distinguishes overloads the
	// abstract composition cannot see (`typename T::A f(T)` vs
	// `typename T::B f(T)` are distinct templates).
	if (PositionalizeTemplateNames(FlattenSpecifierSeq(inner.specifiers),
	                               params) !=
	    PositionalizeTemplateNames(
	        FlattenSpecifierSeq(tmpl.pattern_decl->specifiers),
	        tmpl.params))
		return false;
	return true;
}

bool SemBinder::SameImportedTemplateSignature(TemplateInfo& own,
                                              TemplateInfo& other)
{
	if (own.params.size() != other.params.size())
		return false;
	if (!other.pattern_decl)
		return false;
	return SameFunctionTemplateSignature(own, *other.decl,
	                                     *other.pattern_decl);
}

// Structural deduction of a partial-specialization pattern list
// against concrete arguments: fixed pattern slots unify one-to-one,
// `pattern...` slots absorb their run; trailing concrete arguments
// (defaulted tails) are accepted.
bool SemBinder::DeduceTemplateArgs(const vector<TemplateArg>& pattern,
                                   const vector<TemplateArg>& args,
                                   vector<TemplateArg>& bound,
                                   bool allow_trailing)
{
	return DeduceFromArgList(pattern, args, bound, allow_trailing);
}

// The single-type unification entry for the ordering/participation
// unit (template_order.cpp); the structural rules stay file-local.
bool SemBinder::DeducePatternType(const TypePtr& pattern,
                                  const TypePtr& arg,
                                  vector<TemplateArg>& bound,
                                  bool exact_cv)
{
	return DeduceFromType(pattern, arg, bound, exact_cv);
}

// --- deduction ---------------------------------------------------------------

// The flattened concrete argument list of a deduction result: the
// per-parameter slots with each pack's deduced run spliced into its
// position (a completed slot run wins; the shared trailing-parameter
// run serves the classic single trailing pack).
static vector<TemplateArg> FlattenDeduced(
	const vector<TemplateParam>& params, const vector<TemplateArg>& bound,
	const vector<TemplateArg>& pack_elements)
{
	vector<TemplateArg> flattened;
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].pack)
		{
			const vector<TemplateArg>& run =
				i < bound.size() && bound[i].pack_done
					? bound[i].pack_elements : pack_elements;
			for (size_t k = 0; k < run.size(); k++)
				flattened.push_back(run[k]);
		}
		else
			flattened.push_back(bound[i]);
	}
	return flattened;
}

// 14.8.1: explicit template arguments bind the leading parameters;
// the pack absorbs the remaining explicit arguments. False on any
// unresolvable or dependent argument (the template contributes no
// candidate).
bool SemBinder::BindExplicitDeductionArgs(TemplateInfo& tmpl,
                                          const AstNamePart& part,
                                          vector<TemplateArg>& bound,
                                          vector<TemplateArg>& pack_elements)
{
	size_t cursor = 0;
	for (size_t i = 0; i < part.arguments.size(); i++)
	{
		const AstTemplateArgument& argument = part.arguments[i];
		// An explicit `Args...` expansion splices the bound pack's
		// elements (the instantiated base/body context binds them).
		if (ExplicitArgumentIsExpansion(argument))
		{
			const AstName* pname = ExpansionPackName(argument);
			if (!pname)
				return false;
			const ScopeBinding* pack = UnqualifiedLookup(
				current_, pname->parts[0].identifier, SLF_ANY);
			if (!pack || !pack->is_pack || pack->param_index >= 0)
				return false;
			for (size_t k = 0; k < pack->pack_args.size(); k++)
			{
				const TemplateArg& element = pack->pack_args[k];
				if (TemplateArgIsDependent(element))
					return false;
				if (cursor < tmpl.params.size() &&
				    !tmpl.params[cursor].pack)
					bound[cursor++] = element;
				else
					pack_elements.push_back(element);
			}
			continue;
		}
		if (cursor >= tmpl.params.size())
			return false;
		const TemplateParam& param = tmpl.params[cursor];
		TemplateArg resolved;
		try
		{
			if (param.kind == TPK_TEMPLATE)
			{
				// A template-name argument parses as a type-id
				// spelling just the name (`check<F>(0)`).
				const AstName* tname = 0;
				if (argument.is_type && argument.type &&
				    (!argument.type->declarator ||
				     argument.type->declarator->Empty()) &&
				    argument.type->specifiers.size() == 1 &&
				    argument.type->specifiers[0].kind == SPEC_TYPE_NAME)
					tname = &argument.type->specifiers[0].name;
				if (!tname)
					return false;
				resolved = ResolveTemplateTemplateArgument(*tname, param,
				                                           tmpl.name);
			}
			else if (param.kind == TPK_TYPE)
			{
				if (!argument.is_type || !argument.type)
					return false;
				resolved = TemplateArg(
					builder_.ResolveTypeId(*argument.type));
			}
			else
			{
				Scope* partial = MakeArgumentAliasScope(tmpl, bound);
				resolved = ResolveValueArgument(
					argument, ValueParamType(param, partial));
			}
		}
		catch (const std::exception&)
		{
			return false;
		}
		if (TemplateArgIsDependent(resolved))
			return false;
		if (param.pack)
			pack_elements.push_back(resolved);
		else
			bound[cursor++] = resolved;
	}
	return true;
}

// One fixed (non-pack) parameter pattern against one call argument
// (14.8.2.1 subset with the p3 forwarding-reference rule).
bool SemBinder::DeduceFixedParameter(const TypePtr& pattern,
                                     const SemValue& arg,
                                     vector<TemplateArg>& bound)
{
	if (!pattern)
		return true;  // non-deduced context
	if (!TypeIsDependent(pattern))
		return true;  // ordinary conversion checking applies later
	// 14.8.2.1p6: an overload-set argument tries deduction per
	// member; exactly one success binds, anything else leaves the
	// parameter non-deduced (a set with function templates is
	// non-deduced outright).
	if (arg.function_set && arg.overloads.size() > 1)
	{
		if (!arg.fn_templates.empty())
			return true;
		vector<TemplateArg> chosen;
		size_t successes = 0;
		for (size_t i = 0; i < arg.overloads.size(); i++)
		{
			SemValue shell;
			shell.type = arg.overloads[i];
			shell.category = VC_LVALUE;
			vector<TemplateArg> probe = bound;
			if (DeduceFixedParameter(pattern, shell, probe))
			{
				successes++;
				chosen.swap(probe);
			}
		}
		if (successes == 1)
			bound.swap(chosen);
		return true;
	}
	TypePtr arg_type = arg.type;
	if (!arg_type)
		return false;
	if (IsReferenceType(pattern))
	{
		TypePtr referee = pattern->target;
		// 14.8.2.1p3: a forwarding reference binding an lvalue deduces
		// the parameter as an lvalue reference.
		if (pattern->kind == TK_RVALUE_REFERENCE &&
		    referee->kind == TK_TYPE_PARAM && !referee->is_const &&
		    !referee->is_volatile && arg.category == VC_LVALUE)
		{
			int index = referee->named->param_index;
			if (index < 0 || (size_t)index >= bound.size())
				return false;
			TypePtr as_ref = MakeReferenceType(arg_type, false, true);
			if (ArgBound(bound[index]))
				return bound[index].type &&
					TypeEquals(bound[index].type, as_ref);
			bound[index] = TemplateArg(as_ref);
			return true;
		}
		return DeduceFromType(referee, arg_type, bound, false);
	}
	return DeduceFromType(RemoveTopCv(pattern),
	                      DecayForDeduction(arg_type), bound, false);
}

// One pack element: the element pattern unifies against a copy of the
// bound slots with the pack's slot cleared, so the pack placeholder
// yields the element while fixed parameters keep deducing
// consistently.
bool SemBinder::DeducePackElement(const TypePtr& pattern,
                                  const SemValue& arg, size_t pack_index,
                                  vector<TemplateArg>& bound,
                                  TemplateArg& element)
{
	TypePtr arg_type = arg.type;
	if (!pattern || !arg_type)
		return false;
	if (pattern->kind == TK_RVALUE_REFERENCE && pattern->target &&
	    pattern->target->kind == TK_TYPE_PARAM &&
	    !pattern->target->is_const && !pattern->target->is_volatile &&
	    arg.category == VC_LVALUE)
	{
		element = TemplateArg(MakeReferenceType(arg_type, false, true));
		return true;
	}
	vector<TemplateArg> probe = bound;
	probe[pack_index] = TemplateArg();
	TypePtr pat = pattern;
	TypePtr at = arg_type;
	if (IsReferenceType(pat))
		pat = pat->target;
	else
	{
		pat = RemoveTopCv(pat);
		at = DecayForDeduction(at);
	}
	if (!DeduceFromType(pat, at, probe, false))
		return false;
	if (!ArgBound(probe[pack_index]))
		return false;
	element = probe[pack_index];
	for (size_t j = 0; j < probe.size(); j++)
		if (j != pack_index && !ArgBound(bound[j]) && ArgBound(probe[j]))
			bound[j] = probe[j];
	return true;
}

// Unbound parameters fill from default template arguments (under the
// partially-bound alias scope); any remaining hole or substitution
// failure fails the deduction.
bool SemBinder::FillDeducedDefaults(TemplateInfo& tmpl,
                                    vector<TemplateArg>& bound,
                                    const vector<TemplateArg>& pack_elements)
{
	for (size_t i = 0; i < bound.size(); i++)
	{
		if (tmpl.params[i].pack || ArgBound(bound[i]))
			continue;
		const TemplateParam& param = tmpl.params[i];
		if (!param.default_type && !param.default_expr)
			return false;
		Scope* partial = MakeArgumentAliasScope(
			tmpl, FlattenDeduced(tmpl.params, bound, pack_elements));
		Scope* saved = current_;
		current_ = partial;
		try
		{
			if (param.kind == TPK_TYPE)
				bound[i] = TemplateArg(builder_.ResolveTypeId(
					*param.default_type));
			else
				bound[i] = ResolveDefaultValueExpr(
					*param.default_expr,
					ValueParamType(param, partial));
		}
		catch (const std::exception&)
		{
			current_ = saved;
			return false;
		}
		current_ = saved;
	}
	return true;
}

const FunctionSpecialization* SemBinder::DeduceFunctionTemplate(
	TemplateInfo& tmpl, const vector<SemValue>& args,
	const AstNamePart* explicit_part)
{
	EnsureFunctionPattern(tmpl);
	size_t pack_index = TemplatePackIndex(tmpl.params);
	bool has_pack = pack_index < tmpl.params.size();
	bool pack_pattern_last = !tmpl.param_pattern_packs.empty() &&
		tmpl.param_pattern_packs.back();
	// Arguments beyond the declared parameters match a trailing
	// ellipsis (13.3.2p2); they deduce nothing.
	bool pattern_variadic = tmpl.pattern && tmpl.pattern->variadic;
	if (!pack_pattern_last && !pattern_variadic &&
	    tmpl.param_patterns.size() < args.size())
		return 0;
	vector<TemplateArg> bound(tmpl.params.size());
	// Pack slots absorb runs when a template-id pattern (tuple<T...>)
	// deduces against a specialization's arguments.
	for (size_t i = 0; i < bound.size(); i++)
		bound[i].is_pack_slot = tmpl.params[i].pack;
	vector<TemplateArg> pack_elements;
	if (explicit_part &&
	    !BindExplicitDeductionArgs(tmpl, *explicit_part, bound,
	                               pack_elements))
		return 0;
	// 14.8.2p2: explicit arguments substitute into P before deduction;
	// a parameter type mentioning only explicitly bound slots is a
	// concrete type checked by conversion, not unification.
	vector<bool> explicit_bound(bound.size(), false);
	for (size_t i = 0; i < bound.size(); i++)
		explicit_bound[i] = ArgBound(bound[i]);
	size_t explicit_elements = pack_elements.size();
	size_t deduced_elements = 0;
	size_t p = 0;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (p >= tmpl.param_patterns.size())
		{
			if (pattern_variadic)
				break;  // the remaining arguments ride the ellipsis
			return 0;
		}
		bool pattern_is_pack = p < tmpl.param_pattern_packs.size() &&
			tmpl.param_pattern_packs[p];
		const TypePtr& pattern = tmpl.param_patterns[p];
		if (!pattern_is_pack)
		{
			p++;
			if (!DeduceFixedParameter(pattern, args[i], bound))
			{
				// With every parameter explicitly bound, any pattern
				// is concrete after substitution regardless of its
				// (possibly deferred) shape.
				bool all_params_explicit = true;
				for (size_t s = 0; s < explicit_bound.size(); s++)
					if (!tmpl.params[s].pack && !explicit_bound[s])
						all_params_explicit = false;
				if (!all_params_explicit)
				{
					std::vector<size_t> slots;
					if (!CollectPatternSlots(pattern, slots))
						return 0;
					bool all_explicit = true;
					for (size_t s = 0; s < slots.size(); s++)
						if (slots[s] >= explicit_bound.size() ||
						    !explicit_bound[slots[s]])
							all_explicit = false;
					if (!all_explicit)
						return 0;
				}
			}
			continue;
		}
		// The trailing pack pattern: each remaining argument deduces
		// one element (14.8.2.1p1 last clause).
		TemplateArg element;
		if (!has_pack ||
		    !DeducePackElement(pattern, args[i], pack_index, bound,
		                       element))
			return 0;
		if (deduced_elements < explicit_elements)
		{
			if (!TemplateArgEquals(element,
			                       pack_elements[deduced_elements]))
				return 0;
		}
		else
			pack_elements.push_back(element);
		deduced_elements++;
	}
	// The trailing pack's run completes its slot, so the flatten and
	// the alias scope read every pack from its own slot (multiple
	// deducible packs each carry their own run).
	if (has_pack && !bound[pack_index].pack_done)
	{
		bound[pack_index].pack_done = true;
		bound[pack_index].pack_elements = pack_elements;
	}
	if (!FillDeducedDefaults(tmpl, bound, pack_elements))
		return 0;
	// 14.8.2p8 (PA22): substitution failure while composing the
	// concrete signature (parameter/return substitution, trailing
	// decltype analysis, enable_if member lookup) is an immediate-
	// context failure: the template contributes no candidate. The
	// body instantiates only on odr-use, so body faults never route
	// through here.
	try
	{
		return EnsureFunctionSpecialization(
			tmpl, FlattenDeduced(tmpl.params, bound, pack_elements),
			&bound);
	}
	catch (const std::exception&)
	{
		return 0;
	}
}

// --- specialization -----------------------------------------------------------

// 13.5p6: a namespace-scope operator function must take at least one
// class or enumeration operand, so a specialization over builtin
// operand types only is ill-formed (allocation and literal operators
// are exempt).
void SemBinder::CheckOperatorSpecializationOperands(const TemplateInfo& tmpl,
                                                    const TypePtr& type)
{
	if (tmpl.member_of || tmpl.name.compare(0, 9, "operator ") != 0 ||
	    tmpl.name.compare(0, 10, "operator \"") == 0)
		return;
	const string text = tmpl.name.substr(9);
	if (text == "new" || text == "new[]" || text == "delete" ||
	    text == "delete[]")
		return;
	bool class_or_enum = false;
	bool dependent = false;
	for (size_t i = 0; i < type->parameters.size(); i++)
	{
		TypePtr param = type->parameters[i];
		if (IsReferenceType(param))
			param = param->target;
		param = RemoveTopCv(param);
		if (TypeIsDependent(param))
			dependent = true;
		if (param->kind == TK_CLASS || param->kind == TK_ENUM)
			class_or_enum = true;
	}
	if (!class_or_enum && !dependent)
		throw runtime_error("operator function specialization without "
		                    "a class or enumeration parameter");
}

FunctionSpecialization* SemBinder::EnsureFunctionSpecialization(
	TemplateInfo& tmpl, const vector<TemplateArg>& args,
	const vector<TemplateArg>* slots)
{
	if (slots && slots->size() != tmpl.params.size())
		throw runtime_error("wrong template argument count for " +
		                    tmpl.name);
	std::vector<std::pair<size_t, size_t>> spans;
	if (!slots &&
	    (!MapParamSpans(tmpl.params, args.size(), spans) ||
	     (TemplatePackIndex(tmpl.params) == tmpl.params.size() &&
	      args.size() != tmpl.params.size())))
		throw runtime_error("wrong template argument count for " +
		                    tmpl.name);
	// The slot list keys multi-pack results (the flattened list loses
	// the run boundaries).
	string key = slots ? TemplateArgumentKey(*slots)
	                   : TemplateArgumentKey(args);
	{
		map<string, unique_ptr<FunctionSpecialization>>::iterator found =
			tmpl.fn_specs.find(key);
		if (found != tmpl.fn_specs.end())
			return found->second.get();
	}
	if (instantiation_depth_ >= kTemplateInstantiationDepthLimit)
		throw runtime_error("template instantiation depth limit "
		                    "exceeded for " + tmpl.name);
	// The record enters the cache only after the signature composes:
	// a substitution failure must not leave a half-built entry behind.
	unique_ptr<FunctionSpecialization> fresh(new FunctionSpecialization());
	FunctionSpecialization* spec = fresh.get();
	spec->owner = &tmpl;
	spec->args = args;
	spec->key = key;
	spec->name = tmpl.name + TemplateArgumentSpelling(args);
	spec->param_scope =
		MakeArgumentAliasScope(tmpl, slots ? *slots : args);

	// Compose the concrete signature in the template's context; the
	// parameters bind into a scratch scope so a trailing-return
	// decltype can name them (8.3.5p2).
	const AstDecl& inner = *tmpl.pattern_decl;
	const AstDeclarator* declarator = PatternDeclarator(inner);
	Scope* capture = model_.CreateScope(SCOPE_FUNCTION, tmpl.name,
	                                    spec->param_scope);
	InstantiationContext context(*this, capture);
	param_capture_scope_ = capture;
	// PA21 member templates: a trailing-return decltype may name the
	// enclosing class's members through the implicit this.
	if (tmpl.member_of && !tmpl.member_static)
	{
		method_.cls = unit_.classes.Find(tmpl.member_of);
		if (method_.cls)
			method_.this_type = MakePointerType(
				MakeNamedType(TK_CLASS, tmpl.member_of), false, false);
	}
	PreBindDeclaredParameters(declarator);
	last_pack_param_ = PackParamRecord();
	DeclSpecifierInfo specs;
	if (PatternIsSpecialMember(inner))
		specs.type = MakeFundamentalType(FT_VOID);
	else
		specs = builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed =
		builder_.ComposeDeclarator(declarator, specs.type);
	BindCapturedPackParameter(capture);
	if (!composed.declares_function ||
	    composed.type->kind != TK_FUNCTION)
		throw runtime_error("function template " + tmpl.name +
		                    " does not declare a function");
	CheckOperatorSpecializationOperands(tmpl, composed.type);
	spec->type = composed.type;
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		spec->declared_params.push_back(composed.parameters[i].type);
		spec->param_names.push_back(composed.parameters[i].name);
	}

	spec->self.kind = SB_FUNCTION;
	spec->self.name = spec->name;
	spec->self.type = composed.type;
	spec->self.fn_self_spec = spec;
	// The alias scope is the specialization's identity scope: the
	// lowering keys the definition and its call sites on it, and the
	// canonical-name walk skips it up to the declaring namespace.
	spec->self.owner = spec->param_scope;
	spec->self.home = spec->param_scope;
	spec->self.fn_defaults.resize(1);
	spec->self.fn_deleted.resize(1, false);
	spec->self.fn_access.resize(1, tmpl.member_access);
	spec->self.fn_static.resize(1, tmpl.member_static);
	spec->self.fn_inline_def.resize(1, false);
	spec->self.fn_adl_only.resize(1, false);
	spec->self.fn_unwind_no.resize(1, composed.noexcept_simple);
	spec->self.fn_noexcept_decl.resize(1, composed.noexcept_simple);
	spec->self.fn_owner.resize(1, spec->param_scope);
	vector<const AstExpr*>& defaults = spec->self.fn_defaults[0];
	defaults.resize(composed.parameters.size(), 0);
	for (size_t i = 0; i < composed.parameters.size(); i++)
		defaults[i] = composed.parameters[i].default_arg;

	tmpl.fn_specs[key] = std::move(fresh);
	return spec;
}

const FunctionSpecialization* SemBinder::DeduceFunctionTemplateFromTarget(
	TemplateInfo& tmpl, const TypePtr& target)
{
	EnsureFunctionPattern(tmpl);
	if (!tmpl.pattern || !target || target->kind != TK_FUNCTION)
		return 0;
	vector<TemplateArg> bound(tmpl.params.size());
	if (!DeduceFromType(tmpl.pattern, target, bound))
		return 0;
	for (size_t i = 0; i < bound.size(); i++)
		if (!ArgBound(bound[i]))
			return 0;
	// 14.8.2.2: substitution failure drops the candidate from the
	// overload-set deduction, like call deduction.
	try
	{
		return EnsureFunctionSpecialization(tmpl, bound);
	}
	catch (const std::exception&)
	{
		return 0;
	}
}
