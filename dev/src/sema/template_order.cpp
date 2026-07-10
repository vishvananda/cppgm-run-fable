#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA22 partial ordering and conversion/constructor-template
// participation, split from template_deduce.cpp: the 14.5.6.2/
// 14.8.2.4 transformed-type ordering walks, and the deduction of
// conversion-function and constructor templates against required
// destination/source types (reached through the sem_convert hooks).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA22 assignment boundary");
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
		// 14.5.6.2p3: a deducible bound slot synthesizes a unique
		// value (consistent per slot, improbable as a written bound).
		if (copy.bound_param >= 0)
		{
			copy.bound_known = true;
			copy.bound = 0x7fffff00ull + (size_t)copy.bound_param;
			copy.bound_param = -1;
		}
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

}  // namespace

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
	for (size_t i = 0; i < bound.size(); i++)
		bound[i].is_pack_slot = b.params[i].pack;
	for (size_t i = 0; i < argc; i++)
	{
		// 14.8.2.4p8: an argument transformed from a function
		// parameter pack does not deduce against a non-pack parameter
		// (`f(F&&, Args&&...)` is more specialized than
		// `f(Args&&...)`).
		if (i < a.param_pattern_packs.size() && a.param_pattern_packs[i] &&
		    (i >= b.param_pattern_packs.size() ||
		     !b.param_pattern_packs[i]))
			return false;
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
		if (!DeducePatternType(pattern, transformed, bound))
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
	bool a_at_least = OrderingAtLeastAsSpecialized(*a->owner, *b->owner,
	                                               argc);
	bool b_at_least = OrderingAtLeastAsSpecialized(*b->owner, *a->owner,
	                                               argc);
	if (a_at_least != b_at_least)
		return a_at_least;
	if (!a_at_least)
		return false;
	// 14.8.2.4p9: deduction succeeded in both directions; positions
	// where both parameters were reference types tie-break on the
	// remembered lvalue-ness and cv-qualification. `a` wins when some
	// position prefers it and none prefers `b`.
	TemplateInfo& ta = *a->owner;
	TemplateInfo& tb = *b->owner;
	bool prefer_a = false;
	bool prefer_b = false;
	for (size_t i = 0;
	     i < argc && i < ta.param_patterns.size() &&
	     i < tb.param_patterns.size(); i++)
	{
		const TypePtr& pa = ta.param_patterns[i];
		const TypePtr& pb = tb.param_patterns[i];
		if (!pa || !pb || !IsReferenceType(pa) || !IsReferenceType(pb))
			continue;
		bool a_lref = pa->kind == TK_LVALUE_REFERENCE;
		bool b_lref = pb->kind == TK_LVALUE_REFERENCE;
		if (a_lref != b_lref)
		{
			(a_lref ? prefer_a : prefer_b) = true;
			continue;
		}
		// An array's top-level cv lives on its element type (3.9.3p2).
		TypePtr ra = pa->target;
		TypePtr rb = pb->target;
		while (ra->kind == TK_ARRAY)
			ra = ra->target;
		while (rb->kind == TK_ARRAY)
			rb = rb->target;
		bool a_more_cv = (ra->is_const || !rb->is_const) &&
			(ra->is_volatile || !rb->is_volatile) &&
			(ra->is_const != rb->is_const ||
			 ra->is_volatile != rb->is_volatile);
		bool b_more_cv = (rb->is_const || !ra->is_const) &&
			(rb->is_volatile || !ra->is_volatile) &&
			(ra->is_const != rb->is_const ||
			 ra->is_volatile != rb->is_volatile);
		if (a_more_cv)
			prefer_a = true;
		if (b_more_cv)
			prefer_b = true;
	}
	return prefer_a && !prefer_b;
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
	if (!DeduceTemplateArgs(b.pattern, transformed, bound, false))
		return false;
	// A pack pattern in `b` deducing from a fixed run in `a` (or the
	// reverse absorption) already decided; a leftover fixed slot in
	// `b` only matters for completeness, which ordering ignores.
	return true;
}


// --- conversion-function templates (14.8.2.3) -------------------------------

void SemBinder::CheckArrayElementType(const TypePtr& element)
{
	TypePtr bare = element ? RemoveTopCv(element) : element;
	if (!bare || bare->kind != TK_CLASS)
		return;
	EnsureTypeCompleteness(bare->named);
	const ClassInfo* cls = unit_.classes.Find(bare->named);
	if (!cls)
		return;
	for (size_t i = 0; i < cls->vslots.size(); i++)
		if (cls->vslots[i].kind == VS_METHOD && cls->vslots[i].pure)
			throw runtime_error("array of abstract " +
			                    bare->named->display);
}

void SemBinder::DeduceConversionTemplates(const NamedTypeInfo* entity,
                                          const TypePtr& dest)
{
	ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
	while (cls)
	{
		for (size_t t = 0; t < cls->conversion_templates.size(); t++)
		{
			try
			{
				DeduceOneConversionTemplate(
					*cls->conversion_templates[t], dest, *cls);
			}
			catch (const InstantiationBodyFault&)
			{
				throw;
			}
			catch (const std::exception&)
			{
				// 14.8.2p8: the template contributes no entry.
			}
		}
		cls = cls->base ? unit_.classes.Find(cls->base->entity) : 0;
	}
}

void SemBinder::DeduceCtorTemplatesForConversion(
	const NamedTypeInfo* entity, const ConversionSource& source)
{
	ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
	if (!cls || cls->ctor_templates.empty())
		return;
	vector<SemValue> shells;
	if (source.braced)
	{
		// PA24 list-initialization: the braced elements deduce the
		// class's constructor templates position-wise.
		shells.resize(source.list_items.size());
		for (size_t i = 0; i < source.list_items.size(); i++)
		{
			if (!source.list_items[i].type)
				return;
			shells[i].type = source.list_items[i].type;
			shells[i].category = source.list_items[i].category;
		}
	}
	else
	{
		if (!source.type)
			return;
		shells.resize(1);
		shells[0].type = source.type;
		shells[0].category = source.category;
	}
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	vector<size_t> positions;
	AppendCtorTemplateCandidates(*cls, shells, candidates, min_arity,
	                             positions, 0);
}

void SemBinder::DeduceOneConversionTemplate(TemplateInfo& tmpl,
                                            const TypePtr& dest,
                                            ClassInfo& cls)
{
	const AstDecl& inner = *tmpl.pattern_decl;
	if (!tmpl.conversion_type)
	{
		const AstName* id = inner.declarator ? inner.declarator->IdName()
		                                     : 0;
		if (!id || id->parts.empty() ||
		    id->parts.back().kind != NP_CONVERSION_FUNCTION ||
		    !id->parts.back().conversion_type)
			throw OutsideBoundary("conversion template form");
		tmpl.conversion_type = id->parts.back().conversion_type.get();
	}
	const AstTypeId& conversion_type = *tmpl.conversion_type;
	// The conversion-type-id composes with the parameters bound to
	// the positional placeholders (the deduction pattern P) once per
	// template; the composition doubles as the mangling pattern.
	if (!tmpl.conversion_pattern)
	{
		Scope* scope = MakePatternParamScope(tmpl.params, tmpl.declaring);
		Scope* saved = current_;
		current_ = scope;
		try
		{
			tmpl.conversion_pattern = builder_.ResolveTypeId(
				conversion_type);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	TypePtr pattern = tmpl.conversion_pattern;
	// 14.8.2.3p2-3: reference types deduce through their referees.
	TypePtr a = dest;
	if (IsReferenceType(pattern))
		pattern = pattern->target;
	if (IsReferenceType(a))
		a = a->target;
	vector<TemplateArg> bound(tmpl.params.size());
	if (TypeIsDependent(pattern))
	{
		if (!DeducePatternType(RemoveTopCv(pattern), RemoveTopCv(a),
		                       bound, false))
			throw runtime_error("conversion deduction failed");
	}
	else if (!TypeEquals(RemoveTopCv(pattern), RemoveTopCv(a)))
		throw runtime_error("conversion deduction failed");
	// Unbound parameters fill from defaults; a hole fails deduction.
	for (size_t i = 0; i < bound.size(); i++)
	{
		if (tmpl.params[i].pack || ArgBound(bound[i]))
			continue;
		const TemplateParam& param = tmpl.params[i];
		if (!param.default_type && !param.default_expr)
			throw runtime_error("conversion deduction incomplete");
		Scope* partial = MakeArgumentAliasScope(tmpl, bound);
		Scope* saved = current_;
		current_ = partial;
		try
		{
			if (param.kind == TPK_TYPE)
				bound[i] = TemplateArg(builder_.ResolveTypeId(
					*param.default_type));
			else
				bound[i] = ResolveDefaultValueExpr(
					*param.default_expr, ValueParamType(param, partial));
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
	}
	FunctionSpecialization* spec = EnsureFunctionSpecialization(
		tmpl, FlattenDeduced(tmpl.params, bound,
		                     vector<TemplateArg>()));
	// The special-member composition returned `void() cv`; the real
	// result type composes under the specialization's argument scope.
	if (IsVoidType(spec->type->target))
	{
		TypePtr result;
		Scope* saved = current_;
		current_ = spec->param_scope;
		try
		{
			result = builder_.ResolveTypeId(conversion_type);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		Type fixed = *spec->type;
		fixed.target = result;
		spec->type = TypePtr(new Type(fixed));
		spec->name = "operator " + DescribeType(result);
		spec->self.type = spec->type;
		spec->self.name = spec->name;
	}
	for (size_t i = 0; i < cls.conversions.size(); i++)
		if (cls.conversions[i].spec == spec)
			return;
	ClassConversion conv;
	conv.name = spec->name;
	conv.result = spec->type->target;
	conv.type = spec->type;
	conv.is_explicit = false;
	for (size_t i = 0; i < inner.member_specifiers.size(); i++)
		if (inner.member_specifiers[i].keyword == KW_EXPLICIT)
			conv.is_explicit = true;
	conv.access = tmpl.member_access;
	conv.spec = spec;
	cls.conversions.push_back(conv);
}

