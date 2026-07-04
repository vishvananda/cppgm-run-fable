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

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA18 assignment boundary");
}

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
	// (CheckDependentPatternSlots); it matches anything here.
	if (pattern.dependent_type)
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
		while (entity &&
		       entity->spec_template != pattern->named->spec_template)
			entity = entity->base_entity;
		if (!entity)
			return false;
		return DeduceFromArgList(pattern->targs, entity->spec_args,
		                         bound, false);
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

// 14.5.6.2p3: the transformed parameter type of one partial-ordering
// candidate - every type parameter replaced by its synthesized unique
// type. Null when the pattern has a shape outside the ordering subset.
TypePtr SubstituteOrderingTypes(const TypePtr& pattern,
                                const vector<TypePtr>& uniques)
{
	if (!pattern)
		return pattern;
	if (!TypeIsDependent(pattern))
		return pattern;
	switch (pattern->kind)
	{
	case TK_TYPE_PARAM:
	{
		int index = pattern->named->param_index;
		if (index < 0 || (size_t)index >= uniques.size())
			return TypePtr();
		return MakeCvQualifiedType(uniques[index], pattern->is_const,
		                           pattern->is_volatile);
	}
	case TK_POINTER:
	{
		TypePtr target = SubstituteOrderingTypes(pattern->target,
		                                         uniques);
		if (!target)
			return TypePtr();
		return MakePointerType(target, pattern->is_const,
		                       pattern->is_volatile);
	}
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	{
		TypePtr target = SubstituteOrderingTypes(pattern->target,
		                                         uniques);
		if (!target)
			return TypePtr();
		return MakeReferenceType(target,
		                         pattern->kind == TK_RVALUE_REFERENCE,
		                         false);
	}
	case TK_TEMPLATE_SPEC:
	{
		Type copy = *pattern;
		for (size_t i = 0; i < copy.targs.size(); i++)
		{
			if (copy.targs[i].is_value)
				continue;  // value slots compare by identity
			copy.targs[i].type = SubstituteOrderingTypes(
				copy.targs[i].type, uniques);
			if (!copy.targs[i].type)
				return TypePtr();
		}
		return TypePtr(new Type(copy));
	}
	case TK_ARRAY:
	{
		TypePtr element = SubstituteOrderingTypes(pattern->target,
		                                          uniques);
		if (!element)
			return TypePtr();
		Type copy = *pattern;
		copy.target = element;
		return TypePtr(new Type(copy));
	}
	case TK_FUNCTION:
	{
		Type copy = *pattern;
		copy.target = SubstituteOrderingTypes(pattern->target, uniques);
		if (!copy.target)
			return TypePtr();
		for (size_t i = 0; i < copy.parameters.size(); i++)
		{
			bool was_pack = copy.parameters[i]->pack_expansion;
			copy.parameters[i] = SubstituteOrderingTypes(
				copy.parameters[i], uniques);
			if (!copy.parameters[i])
				return TypePtr();
			if (was_pack && !copy.parameters[i]->pack_expansion)
			{
				Type marked = *copy.parameters[i];
				marked.pack_expansion = true;
				copy.parameters[i] = TypePtr(new Type(marked));
			}
		}
		return TypePtr(new Type(copy));
	}
	case TK_MEMBER_POINTER:
	{
		TypePtr member = SubstituteOrderingTypes(pattern->target,
		                                         uniques);
		if (!member)
			return TypePtr();
		Type copy = *pattern;
		copy.target = member;
		return TypePtr(new Type(copy));
	}
	default:
		return TypePtr();
	}
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

TypePtr SemBinder::OrderingUniqueType(size_t index)
{
	while (ordering_uniques_.size() <= index)
	{
		NamedTypeInfo* info = model_.CreateNamedTypeInfo(
			"struct #unique" + std::to_string(ordering_uniques_.size()),
			model_.global(),
			"#unique" + std::to_string(ordering_uniques_.size()));
		ordering_uniques_.push_back(MakeNamedType(TK_CLASS, info));
	}
	return ordering_uniques_[index];
}

// 14.5.6.2p8 (subset): `a` is at least as specialized as `b` for the
// leading `argc` parameters when `b`'s parameters deduce from `a`'s
// transformed parameter types.
bool SemBinder::OrderingAtLeastAsSpecialized(TemplateInfo& a,
                                             TemplateInfo& b,
                                             size_t argc)
{
	EnsureFunctionPattern(a);
	EnsureFunctionPattern(b);
	if (a.param_patterns.size() < argc || b.param_patterns.size() < argc)
		return false;
	vector<TypePtr> uniques;
	for (size_t i = 0; i < a.params.size(); i++)
		uniques.push_back(OrderingUniqueType(i));
	vector<TemplateArg> bound(b.params.size());
	for (size_t i = 0; i < argc; i++)
	{
		TypePtr transformed = SubstituteOrderingTypes(
			a.param_patterns[i], uniques);
		TypePtr pattern = b.param_patterns[i];
		if (!transformed || !pattern)
			return false;
		// 14.8.2.4p5-7: references and top-level cv-qualifiers drop
		// on both sides before deduction.
		if (IsReferenceType(pattern))
			pattern = pattern->target;
		if (IsReferenceType(transformed))
			transformed = transformed->target;
		pattern = RemoveTopCv(pattern);
		transformed = RemoveTopCv(transformed);
		if (!TypeIsDependent(pattern))
		{
			if (!TypeEquals(pattern, transformed))
				return false;
			continue;
		}
		if (!DeduceFromType(pattern, transformed, bound))
			return false;
	}
	return true;
}

bool SemBinder::TemplateCandidateMoreSpecialized(
	const FunctionSpecialization* a, const FunctionSpecialization* b,
	size_t argc)
{
	if (!a || !b || !a->owner || !b->owner || a->owner == b->owner)
		return false;
	return OrderingAtLeastAsSpecialized(*a->owner, *b->owner, argc) &&
		!OrderingAtLeastAsSpecialized(*b->owner, *a->owner, argc);
}

// Composes the declarator of a function-template declaration with the
// parameters bound to the positional placeholders. Returns false when
// the signature does not compose abstractly (dependent qualified
// forms); per-parameter patterns then compose individually.
bool SemBinder::ComposeFunctionPattern(
	const vector<TemplateParam>& params, Scope* declaring,
	const AstDecl& inner, TypePtr& full, vector<TypePtr>& param_patterns,
	vector<bool>& pattern_packs)
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
		// param_index marks the abstract pattern binding (packs stay
		// unexpanded through it).
		binding.param_index = (int)i;
		binding.is_pack = params[i].pack;
		AddBinding(*scope, binding);
	}
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
	ComposeFunctionPattern(tmpl.params, tmpl.declaring,
	                       *tmpl.pattern_decl, tmpl.pattern,
	                       tmpl.param_patterns,
	                       tmpl.param_pattern_packs);
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
	bool composed = ComposeFunctionPattern(params, tmpl.declaring, inner,
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

// 14.5.5.2 (subset): `a` is at least as specialized as `b` when `b`'s
// argument pattern deduces from `a`'s pattern with `a`'s type
// placeholders replaced by synthesized unique types. Value slots keep
// their slot identity: a concrete value in `b` never deduces from a
// slot in `a` (so `X<7>` beats `X<N>`), and repeated-slot consistency
// falls out of slot-index equality. Shapes outside the ordering
// subset are conservatively not-at-least-as-specialized.
bool SemBinder::PartialAtLeastAsSpecialized(const PartialSpecialization& a,
                                            const PartialSpecialization& b)
{
	vector<TypePtr> uniques;
	for (size_t i = 0; i < a.params.size(); i++)
		uniques.push_back(OrderingUniqueType(i));
	vector<TemplateArg> transformed;
	for (size_t i = 0; i < a.pattern.size(); i++)
	{
		TemplateArg arg = a.pattern[i];
		if (!arg.is_value && arg.type)
		{
			arg.type = SubstituteOrderingTypes(arg.type, uniques);
			if (!arg.type)
				return false;
		}
		else if (arg.pack_pattern && !arg.is_value)
			return false;
		transformed.push_back(arg);
	}
	vector<TemplateArg> bound(b.params.size());
	for (size_t i = 0; i < b.params.size(); i++)
		bound[i].is_pack_slot = b.params[i].pack;
	if (!DeduceFromArgList(b.pattern, transformed, bound, false))
		return false;
	// A pack pattern in `b` deducing from a fixed run in `a` (or the
	// reverse absorption) already decided; a leftover fixed slot in
	// `b` only matters for completeness, which ordering ignores.
	return true;
}

// --- deduction ---------------------------------------------------------------

// The flattened concrete argument list of a deduction result: the
// per-parameter slots with the pack's deduced elements spliced into
// its position.
static vector<TemplateArg> FlattenDeduced(
	const vector<TemplateParam>& params, const vector<TemplateArg>& bound,
	const vector<TemplateArg>& pack_elements)
{
	vector<TemplateArg> flattened;
	for (size_t i = 0; i < params.size(); i++)
	{
		if (params[i].pack)
			for (size_t k = 0; k < pack_elements.size(); k++)
				flattened.push_back(pack_elements[k]);
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
		if (argument.pack || cursor >= tmpl.params.size())
			return false;
		const TemplateParam& param = tmpl.params[cursor];
		TemplateArg resolved;
		try
		{
			if (param.kind == TPK_TYPE)
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

const FunctionSpecialization* SemBinder::DeduceFunctionTemplate(
	TemplateInfo& tmpl, const vector<SemValue>& args,
	const AstNamePart* explicit_part)
{
	EnsureFunctionPattern(tmpl);
	size_t pack_index = TemplatePackIndex(tmpl.params);
	bool has_pack = pack_index < tmpl.params.size();
	bool pack_pattern_last = !tmpl.param_pattern_packs.empty() &&
		tmpl.param_pattern_packs.back();
	if (!pack_pattern_last && tmpl.param_patterns.size() < args.size())
		return 0;
	vector<TemplateArg> bound(tmpl.params.size());
	vector<TemplateArg> pack_elements;
	if (explicit_part &&
	    !BindExplicitDeductionArgs(tmpl, *explicit_part, bound,
	                               pack_elements))
		return 0;
	size_t explicit_elements = pack_elements.size();
	size_t deduced_elements = 0;
	size_t p = 0;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (p >= tmpl.param_patterns.size())
			return 0;
		bool pattern_is_pack = p < tmpl.param_pattern_packs.size() &&
			tmpl.param_pattern_packs[p];
		const TypePtr& pattern = tmpl.param_patterns[p];
		if (!pattern_is_pack)
		{
			p++;
			if (!DeduceFixedParameter(pattern, args[i], bound))
				return 0;
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
	// Unbound parameters fill from default template arguments; any
	// remaining hole is a deduction failure.
	for (size_t i = 0; i < bound.size(); i++)
	{
		if (tmpl.params[i].pack || ArgBound(bound[i]))
			continue;
		const TemplateParam& param = tmpl.params[i];
		if (!param.default_type && !param.default_expr)
			return 0;
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
			return 0;
		}
		current_ = saved;
	}
	// 14.8.2p8 (PA22): substitution failure while composing the
	// concrete signature (parameter/return substitution, trailing
	// decltype analysis, enable_if member lookup) is an immediate-
	// context failure: the template contributes no candidate. The
	// body instantiates only on odr-use, so body faults never route
	// through here.
	try
	{
		return EnsureFunctionSpecialization(
			tmpl, FlattenDeduced(tmpl.params, bound, pack_elements));
	}
	catch (const std::exception&)
	{
		return 0;
	}
}

// --- specialization -----------------------------------------------------------

FunctionSpecialization* SemBinder::EnsureFunctionSpecialization(
	TemplateInfo& tmpl, const vector<TemplateArg>& args)
{
	std::vector<std::pair<size_t, size_t>> spans;
	if (!MapParamSpans(tmpl.params, args.size(), spans) ||
	    (TemplatePackIndex(tmpl.params) == tmpl.params.size() &&
	     args.size() != tmpl.params.size()))
		throw runtime_error("wrong template argument count for " +
		                    tmpl.name);
	string key = TemplateArgumentKey(args);
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
	spec->param_scope = MakeArgumentAliasScope(tmpl, args);

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
	// 13.5p6: a namespace-scope operator function must take at least
	// one class or enumeration operand, so a specialization over
	// builtin operand types only is ill-formed (allocation and
	// literal operators are exempt).
	if (!tmpl.member_of && tmpl.name.compare(0, 9, "operator ") == 0 &&
	    tmpl.name.compare(0, 10, "operator \"") != 0)
	{
		const string text = tmpl.name.substr(9);
		if (text != "new" && text != "new[]" && text != "delete" &&
		    text != "delete[]")
		{
			bool class_or_enum = false;
			bool dependent = false;
			for (size_t i = 0;
			     i < composed.type->parameters.size(); i++)
			{
				TypePtr param = composed.type->parameters[i];
				if (IsReferenceType(param))
					param = param->target;
				param = RemoveTopCv(param);
				if (TypeIsDependent(param))
					dependent = true;
				if (param->kind == TK_CLASS || param->kind == TK_ENUM)
					class_or_enum = true;
			}
			if (!class_or_enum && !dependent)
				throw runtime_error(
					"operator function specialization without a "
					"class or enumeration parameter");
		}
	}
	spec->type = composed.type;
	for (size_t i = 0; i < composed.parameters.size(); i++)
		spec->declared_params.push_back(composed.parameters[i].type);

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
