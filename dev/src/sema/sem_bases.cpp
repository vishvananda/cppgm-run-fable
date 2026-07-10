#include "sema/sem_binder.h"

#include <stdexcept>

using std::runtime_error;

// PA26 base-clause binding: resolves the direct-base list (packs
// expanded, access recorded), populates the class's direct-base table
// and the member-lookup base links, and re-seeds the layout with the
// resolved base subobjects.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA26 assignment boundary");
}

}  // namespace

void SemBinder::BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
                               Scope* scope)
{
	(void)info;
	ClassInfo* cls = OpenClass();
	// PA26: base-specifier packs expand per element; every resolved
	// base joins the direct-base table in declaration order. The first
	// base carries the primary chain (vtable slots, the polymorphic
	// layer); extra bases must stay non-polymorphic.
	std::vector<TypePtr> base_types;
	std::vector<const AstBaseSpecifier*> base_specs;
	bool saved_implicit = in_implicit_type_context_;
	in_implicit_type_context_ = true;
	bool any_dependent = false;
	try
	{
		for (size_t b = 0; b < decl.bases.size(); b++)
		{
			const AstBaseSpecifier& spec = decl.bases[b];
			if (spec.is_virtual)
				throw OutsideBoundary("virtual inheritance");
			if (BaseClauseIsDependent(spec.name))
				any_dependent = true;
			if (spec.pack)
				ExpandPackBases(spec, base_types);
			else
				base_types.push_back(ResolveTypeName(spec.name));
			base_specs.resize(base_types.size(), &spec);
		}
	}
	catch (...)
	{
		in_implicit_type_context_ = saved_implicit;
		throw;
	}
	in_implicit_type_context_ = saved_implicit;
	if (base_types.empty())
	{
		// A pack that expanded to nothing: the class stays baseless
		// (the OnClassOpened layout already began).
		if (instantiating_ && any_dependent)
			scope->base_dependent = true;
		return;
	}
	for (size_t b = 0; b < base_types.size(); b++)
	{
		TypePtr base_type = base_types[b];
		if (base_type->kind == TK_CLASS)
			EnsureTypeCompleteness(base_type->named);
		if (base_type->kind != TK_CLASS || !base_type->named->complete)
			throw runtime_error("base class is not a complete class");
		ClassInfo* base_cls = unit_.classes.Find(base_type->named);
		if (!base_cls)
			throw runtime_error("base class record missing");
		// 10.1p3: a class may not appear twice as a direct base.
		for (size_t prior = 0; prior < b; prior++)
			if (base_types[prior]->named == base_type->named)
				throw runtime_error("duplicate direct base class");
		if (b > 0 && base_cls->is_polymorphic)
			throw OutsideBoundary("polymorphic multiple inheritance");
		const AstBaseSpecifier& spec = *base_specs[b];
		ClassDirectBase row;
		row.cls = base_cls;
		// 11.2p2: the default base access is private for `class` keys
		// and public otherwise.
		if (spec.has_access)
			row.access = spec.access == KW_PRIVATE ? MA_PRIVATE
				: spec.access == KW_PROTECTED ? MA_PROTECTED : MA_PUBLIC;
		else
			row.access =
				decl.class_key == KW_CLASS ? MA_PRIVATE : MA_PUBLIC;
		cls->direct_bases.push_back(row);
		if (b == 0)
		{
			cls->base = base_cls;
			cls->base_access = row.access;
			model_.MutableInfo(info)->base_entity = base_cls->entity;
			// Base members become reachable through unqualified and
			// qualified member lookup (10.2 over the base DAG).
			scope->class_base = base_cls->members;
		}
		else
			scope->class_extra_bases.push_back(base_cls->members);
	}
	// 8.5.1p1: a class with bases is not an aggregate.
	cls->is_aggregate = false;
	// PA18 14.6.2p3: a base spelled with a template parameter is
	// dependent; the instantiated scope remembers it so unqualified
	// lookup skips the base subtree.
	if (instantiating_ && any_dependent)
		scope->base_dependent = true;
	// PA17: the derived class inherits the first base's vtable slots
	// (an overrider replaces in place) and the virtual-destructor
	// facts. Introducing the first vpointer over a non-empty
	// non-polymorphic base would need base-subobject pointer
	// adjustment (out of scope).
	cls->vslots = cls->base->vslots;
	cls->dtor_slot = cls->base->dtor_slot;
	if (cls->declares_virtual && !cls->base->is_polymorphic)
		for (size_t b = 0; b < cls->direct_bases.size(); b++)
			if (!cls->direct_bases[b].cls->is_empty)
				throw OutsideBoundary(
					"vpointer introduction over a non-empty "
					"non-polymorphic base");
	BeginClassLayout(*cls);
}
