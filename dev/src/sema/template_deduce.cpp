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
		return DeduceFromType(pattern->target, arg->target, bound);
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
                                              const AstDecl& inner)
{
	EnsureFunctionPattern(tmpl);
	vector<TemplateParam> params;
	CollectTemplateParams(decl, params);
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
	// Substitution failure is a hard error (SFINAE candidate dropping
	// is out of scope); the body instantiates only on odr-use.
	return EnsureFunctionSpecialization(
		tmpl, FlattenDeduced(tmpl.params, bound, pack_elements));
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

// 14.7.1p2: the first odr-use of a deduced specialization
// instantiates its body; a use before the definition re-checks when
// the definition is captured, and the end-of-unit pass errors on
// odr-used specializations that never gained one.
void SemBinder::OnSpecializationOdrUsed(const FunctionSpecialization* spec)
{
	// 3.2p2: names in unevaluated operands are not odr-used.
	if (!spec || in_unevaluated_operand_)
		return;
	FunctionSpecialization& used =
		*const_cast<FunctionSpecialization*>(spec);
	used.odr_used = true;
	if (used.owner && used.owner->has_definition && !used.body_emitted)
		InstantiateFunctionBody(*used.owner, used);
}

void SemBinder::InstantiateFunctionBody(TemplateInfo& tmpl,
                                        FunctionSpecialization& spec)
{
	if (spec.body_emitted)
		return;
	// Set before the bind as the recursion guard (a recursive call in
	// the body finds its own specialization already in progress); a
	// bind failure propagates as a hard error, so the flag never
	// outlives a failed instantiation.
	// An explicit specialization's own definition replaces the primary
	// pattern (14.7.3); a use before it deduced only the signature.
	spec.body_emitted = true;
	const AstDecl& inner = spec.explicit_def ? *spec.explicit_def
	                                         : *tmpl.pattern_decl;
	if (inner.kind != DK_FUNCTION || !inner.body)
		throw runtime_error("function template " + tmpl.name +
		                    " has no definition");
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, spec.name,
	                                     spec.param_scope);
	fn_scope->fn_type = spec.type;
	InstantiationContext context(*this, fn_scope, true);
	param_capture_scope_ = fn_scope;
	// PA21 member templates: the trailing-return decltype may name
	// the enclosing class's members through the implicit this.
	if (tmpl.member_of && !tmpl.member_static)
	{
		method_.cls = unit_.classes.Find(tmpl.member_of);
		if (method_.cls)
			method_.this_type = MakePointerType(
				MakeNamedType(TK_CLASS, tmpl.member_of), false, false);
	}
	// Pre-bind the parameters so the trailing-return decltype (which
	// composes before the clause, 8.3.5p2) can name them, then
	// re-compose the declarator in this specialization's context.
	PreBindDeclaredParameters(inner.declarator.get());
	last_pack_param_ = PackParamRecord();
	DeclSpecifierInfo specs =
		builder_.ProcessSpecifiers(inner.specifiers, true);
	DeclaratorInfo composed = builder_.ComposeDeclarator(
		inner.declarator.get(), specs.type);
	BindCapturedPackParameter(fn_scope);

	SemNodePtr item = MakeSemNode(SN_FUNCTION_DEFINITION);
	SemNode* node = item.get();
	item->name = CanonicalQualifiedName(tmpl.declaring, spec.name);
	item->type = composed.type;
	item->entity_scope = spec.param_scope;
	item->entity_name = spec.name;
	item->unwind_no = composed.noexcept_simple;
	// An explicit specialization is an ordinary definition: strong
	// unless declared inline (7.1.2); instantiated bodies stay weak.
	item->inline_def = !spec.explicit_def || spec.explicit_inline;
	item->fn_spec = &spec;
	// 7.1.5: constexpr on the pattern (or explicit-specialization)
	// declaration makes the instantiated body engine-evaluable and
	// implicitly inline.
	if (DeclHasConstexpr(inner))
	{
		item->is_constexpr_fn = true;
		item->inline_def = true;
	}
	// PA21 member function templates: the body binds as a method (the
	// implicit object parameter first, the class as `this` context).
	const ClassInfo* member_cls = 0;
	if (tmpl.member_of && !tmpl.member_static)
		member_cls = unit_.classes.Find(tmpl.member_of);
	if (member_cls)
	{
		item->type = MethodAdjustedType(*member_cls, composed.type);
		SemNodePtr this_param = MakeSemNode(SN_PARAMETER);
		this_param->name = "this";
		this_param->type = item->type->parameters[0];
		this_param->entity_scope = fn_scope;
		this_param->entity_name = "this";
		item->children.push_back(std::move(this_param));
		if (!FindOwnBinding(*fn_scope, "this"))
		{
			ScopeBinding this_binding;
			this_binding.kind = SB_PARAMETER;
			this_binding.name = "this";
			this_binding.type = item->type->parameters[0];
			AddBinding(*fn_scope, this_binding);
		}
	}
	// Slot names follow the first declaration (the primary pattern);
	// an explicit definition's renamed parameters redirect their body
	// bindings onto the primary-named slots.
	vector<string> slot_names(composed.parameters.size());
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		slot_names[i] = composed.parameters[i].name;
		// Unnamed definition parameters fall back to the first
		// declaration's names.
		if (slot_names[i].empty() &&
		    i < tmpl.declared_param_names.size())
			slot_names[i] = tmpl.declared_param_names[i];
	}
	if (spec.explicit_def && tmpl.pattern_decl &&
	    tmpl.pattern_decl->declarator)
		RedirectExplicitSlotNames(*tmpl.pattern_decl->declarator,
		                          fn_scope, slot_names);
	for (size_t i = 0; i < composed.parameters.size(); i++)
	{
		SemNodePtr parameter = MakeSemNode(SN_PARAMETER);
		parameter->name = slot_names[i];
		parameter->type = composed.parameters[i].type;
		parameter->entity_scope = fn_scope;
		parameter->entity_name = parameter->name;
		AttachParameterDtor(*parameter);
		item->children.push_back(std::move(parameter));
	}
	current_ = fn_scope;
	method_.fn_scope = fn_scope;
	method_.fn_owner = tmpl.declaring;
	method_.fn_name = spec.name;
	method_.fn_template_name = tmpl.name;
	if (tmpl.member_of)
	{
		method_.cls = unit_.classes.Find(tmpl.member_of);
		if (member_cls)
			method_.this_type = item->type->parameters[0];
	}
	current_return_ = composed.type->target;
	parents_.push_back(node);
	try
	{
		BindStatement(*inner.body);
	}
	catch (...)
	{
		// A failed instantiation leaves no trace: a poisoning caller
		// (an eagerly bound sibling body) may retry later, and the
		// odr-use that triggered this bind dies with it.
		spec.body_emitted = false;
		spec.odr_used = false;
		throw;
	}
	parents_.pop_back();

	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	if (!may_throw)
	{
		node->unwind_no = true;
		spec.self.fn_unwind_no[0] = true;
	}
	if (item->inline_def)
		unit_.deferred.push_back(std::move(item));
	else
		unit_.items.push_back(std::move(item));
}

// An explicit specialization's renamed parameters redirect their
// body bindings onto the primary declaration's slot names (the
// lowering resolves slots by the primary spelling).
void SemBinder::RedirectExplicitSlotNames(const AstDeclarator& primary,
                                          Scope* fn_scope,
                                          vector<string>& slot_names)
{
	const AstParameterClause* primary_clause =
		FunctionParameterClause(primary);
	for (size_t i = 0;
	     primary_clause && i < primary_clause->parameters.size() &&
	     i < slot_names.size(); i++)
	{
		const AstDeclarator* pd =
			primary_clause->parameters[i].declarator.get();
		const AstName* pid = pd ? pd->IdName() : 0;
		if (!pid || !pid->IsPlainIdentifier())
			continue;
		const string& primary_name = pid->parts[0].identifier;
		if (primary_name.empty() || primary_name == slot_names[i])
			continue;
		if (!slot_names[i].empty())
			if (ScopeBinding* redirect =
			        FindOwnBinding(*fn_scope, slot_names[i]))
			{
				redirect->pack_element_name = primary_name;
				ScopeBinding slot_binding;
				slot_binding.kind = SB_PARAMETER;
				slot_binding.name = primary_name;
				slot_binding.type = redirect->type;
				AddBinding(*fn_scope, slot_binding);
			}
		slot_names[i] = primary_name;
	}
}

void SemBinder::PreBindDeclaredParameters(const AstDeclarator* declarator)
{
	if (!declarator)
		return;
	const AstParameterClause* clause = FunctionParameterClause(*declarator);
	if (!clause)
		return;
	for (size_t i = 0; i < clause->parameters.size(); i++)
	{
		try
		{
			DeclSpecifierInfo pspecs = builder_.ProcessSpecifiers(
				clause->parameters[i].specifiers, false);
			DeclaratorInfo pcomposed = builder_.ComposeDeclarator(
				clause->parameters[i].declarator.get(), pspecs.type);
			if (pcomposed.id && pcomposed.id->IsPlainIdentifier())
				OnParameterComposed(pcomposed.id->parts[0].identifier,
				                    pcomposed.type);
		}
		catch (const std::exception&)
		{
			// The full composition reports real errors.
		}
	}
}

void SemBinder::InstantiatePendingFunctions(TemplateInfo& tmpl)
{
	for (map<string, unique_ptr<FunctionSpecialization>>::iterator it =
	         tmpl.fn_specs.begin();
	     it != tmpl.fn_specs.end(); ++it)
		if (it->second->odr_used && !it->second->body_emitted)
			InstantiateFunctionBody(tmpl, *it->second);
}

// --- explicit forms ------------------------------------------------------------

const ScopeBinding* SemBinder::ResolveFunctionTemplateId(
	const ScopeBinding& binding, const AstNamePart& part)
{
	// Explicit arguments select among the templates of this name; the
	// PA18 subset requires the argument list (with defaults) to bind
	// every parameter. An argument list that does not bind them all is
	// not an error here: a partial-explicit template-id falls back to
	// the overload set so the call context deduces the rest (14.8.1).
	const FunctionSpecialization* resolved = 0;
	for (size_t t = 0; t < binding.fn_templates.size(); t++)
	{
		TemplateInfo& tmpl = *binding.fn_templates[t];
		// A template with a parameter pack defers to the call context:
		// the pack may still gain elements from argument deduction
		// (14.8.1 with a trailing pack).
		if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
			continue;
		vector<TemplateArg> args;
		try
		{
			args = ResolveTemplateArgumentList(tmpl, part);
		}
		catch (const std::exception&)
		{
			continue;
		}
		bool dependent = false;
		for (size_t i = 0; i < args.size(); i++)
			if ((!args[i].is_value && !args[i].type) ||
			    TemplateArgIsDependent(args[i]))
				dependent = true;
		if (dependent)
			continue;
		const FunctionSpecialization* spec =
			EnsureFunctionSpecialization(tmpl, args);
		if (resolved && resolved != spec)
			throw runtime_error("ambiguous template-id " +
			                    part.identifier);
		resolved = spec;
	}
	if (!resolved)
		// A call context can still deduce the remaining parameters
		// from the arguments (14.8.1); hand back the overload set.
		return &binding;
	return &resolved->self;
}

void SemBinder::BindExplicitFunctionInstantiation(const AstDecl& inner,
                                                  bool is_extern)
{
	// `template void f<int>(int);` / `template void f(int);`: resolve
	// the declarator against the visible function templates.
	if (inner.declarators.size() != 1 ||
	    !inner.declarators[0].declarator)
		throw OutsideBoundary("explicit instantiation declarator");
	const AstName* id = inner.declarators[0].declarator->IdName();
	if (!id || id->parts.empty())
		throw OutsideBoundary("explicit instantiation declarator");
	const AstNamePart& terminal = id->parts.back();
	if (terminal.kind != NP_IDENTIFIER &&
	    terminal.kind != NP_TEMPLATE_ID &&
	    terminal.kind != NP_OPERATOR_FUNCTION)
		throw OutsideBoundary("explicit instantiation name form");
	const string terminal_name = terminal.kind == NP_TEMPLATE_ID
		? terminal.identifier : DeclaredFunctionName(terminal);
	// Resolve the (possibly qualified) name to its function binding.
	Scope* prefix = 0;
	if (id->parts.size() > 1 || id->global_scope)
		prefix = ResolvePrefixScope(*id);
	const ScopeBinding* binding = prefix
		? QualifiedLookup(*prefix, terminal_name, SLF_ANY)
		: UnqualifiedLookup(current_, terminal_name, SLF_ANY);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("explicit instantiation of a non-function");
	// A member of a class-template specialization (`template int
	// tester<int>::test();`): the member's already-instantiated
	// definition becomes an unconditional emission root.
	if (binding->fn_templates.empty() && prefix &&
	    prefix->kind == SCOPE_CLASS)
	{
		if (is_extern)
			return;
		SemUnit::ExplicitMemberInstantiation record;
		record.scope = prefix;
		record.name = terminal_name;
		unit_.explicit_member_instantiations.push_back(record);
		return;
	}
	// The declared signature disambiguates same-name templates whose
	// explicit argument lists would both resolve.
	TypePtr declared;
	try
	{
		DeclSpecifierInfo specs =
			builder_.ProcessSpecifiers(inner.specifiers, true);
		DeclaratorInfo composed = builder_.ComposeDeclarator(
			inner.declarators[0].declarator.get(), specs.type);
		if (composed.type->kind == TK_FUNCTION)
			declared = composed.type;
	}
	catch (const std::exception&)
	{
		declared = TypePtr();
	}
	const FunctionSpecialization* resolved = 0;
	bool has_args = terminal.kind == NP_TEMPLATE_ID ||
		!terminal.arguments.empty();
	if (has_args)
	{
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !resolved; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			if (TemplatePackIndex(tmpl.params) < tmpl.params.size())
				continue;
			vector<TemplateArg> args;
			try
			{
				args = ResolveTemplateArgumentList(tmpl, terminal);
			}
			catch (const std::exception&)
			{
				continue;
			}
			FunctionSpecialization* spec;
			try
			{
				spec = EnsureFunctionSpecialization(tmpl, args);
			}
			catch (const std::exception&)
			{
				continue;
			}
			if (declared && !TypeEquals(spec->type, declared))
				continue;
			resolved = spec;
		}
	}
	else
	{
		if (!declared)
			throw runtime_error("explicit instantiation of a "
			                    "non-function");
		// Deduce the template arguments from the declared types.
		for (size_t t = 0;
		     t < binding->fn_templates.size() && !resolved; t++)
		{
			TemplateInfo& tmpl = *binding->fn_templates[t];
			EnsureFunctionPattern(tmpl);
			if (!tmpl.pattern)
				continue;
			vector<TemplateArg> bound(tmpl.params.size());
			if (!DeduceFromType(tmpl.pattern, declared, bound))
				continue;
			bool complete = true;
			for (size_t i = 0; i < bound.size(); i++)
				if (!ArgBound(bound[i]))
					complete = false;
			if (!complete)
				continue;
			resolved = EnsureFunctionSpecialization(tmpl, bound);
		}
	}
	if (!resolved)
		throw runtime_error("explicit instantiation matches no template");
	FunctionSpecialization& spec =
		*const_cast<FunctionSpecialization*>(resolved);
	if (is_extern)
	{
		// 14.7.2p10: the definition lives in another translation unit;
		// local uses reference the strong external symbol.
		spec.extern_suppressed = true;
		unit_.extern_fn_suppressions.push_back(&spec);
		return;
	}
	// 14.7.2p8: the explicit-instantiation definition demands the body
	// and emits it unconditionally (an emission root); it also lifts a
	// preceding extern declaration's suppression.
	spec.extern_suppressed = false;
	spec.inst_definition = true;
	OnSpecializationOdrUsed(&spec);
	unit_.explicit_fn_instantiations.push_back(&spec);
}

void SemBinder::OnParameterComposed(const string& name,
                                    const TypePtr& type)
{
	if (!param_capture_scope_ || name.empty())
		return;
	if (FindOwnBinding(*param_capture_scope_, name))
		return;
	ScopeBinding binding;
	binding.kind = SB_PARAMETER;
	binding.name = name;
	// 8.3.5p5: an array- or function-typed parameter declares the
	// adjusted pointer object.
	binding.type = type->kind == TK_ARRAY || type->kind == TK_FUNCTION
		? AdjustParameterType(type) : type;
	AddBinding(*param_capture_scope_, binding);
}

Scope* SemBinder::SwapLookupScope(Scope* scope)
{
	Scope* previous = current_;
	current_ = scope;
	return previous;
}

void SemBinder::RequireCompleteType(const NamedTypeInfo* info)
{
	EnsureTypeCompleteness(info);
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
	// Substitution failure is a hard error (no SFINAE dropping).
	return EnsureFunctionSpecialization(tmpl, bound);
}
