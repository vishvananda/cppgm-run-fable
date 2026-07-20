#include "sema/sem_binder.h"

#include <stdexcept>

using std::runtime_error;

// PA27 base-clause binding: resolves the direct-base list (packs
// expanded, access recorded, virtual bases accepted), populates the
// class's direct-base table and the member-lookup base links, selects
// the primary base (the first polymorphic non-virtual base carries the
// vtable chain), and re-seeds the layout with the resolved subobjects.

void SemBinder::BindBaseClause(const AstDecl& decl, NamedTypeInfo* info,
                               Scope* scope)
{
	(void)info;
	ClassInfo* cls = OpenClass();
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
			if (BaseClauseIsDependent(spec.name))
				any_dependent = true;
			if (spec.pack)
				ExpandPackBases(spec, base_types);
			else
				base_types.push_back(ResolveTypeName(spec.name));
			base_specs.resize(base_types.size(), &spec);
		}
		(void)0;
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
		const AstBaseSpecifier& spec = *base_specs[b];
		ClassDirectBase row;
		row.cls = base_cls;
		row.is_virtual = spec.is_virtual;
		// 11.2p2: the default base access is private for `class` keys
		// and public otherwise.
		if (spec.has_access)
			row.access = spec.access == KW_PRIVATE ? MA_PRIVATE
				: spec.access == KW_PROTECTED ? MA_PROTECTED : MA_PUBLIC;
		else
			row.access =
				decl.class_key == KW_CLASS ? MA_PRIVATE : MA_PUBLIC;
		cls->direct_bases.push_back(row);
	}
	// PA27 primary selection: the first polymorphic non-virtual base
	// (it shares the vpointer at offset 0), else the first non-virtual
	// base. All other bases join the extra lookup links.
	cls->primary_base = -1;
	for (size_t b = 0; b < cls->direct_bases.size(); b++)
		if (!cls->direct_bases[b].is_virtual &&
		    cls->direct_bases[b].cls->is_polymorphic)
		{
			cls->primary_base = (int)b;
			break;
		}
	if (cls->primary_base < 0)
		for (size_t b = 0; b < cls->direct_bases.size(); b++)
			if (!cls->direct_bases[b].is_virtual)
			{
				cls->primary_base = (int)b;
				break;
			}
	for (size_t b = 0; b < cls->direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls->direct_bases[b];
		if ((int)b == cls->primary_base)
		{
			cls->base = row.cls;
			cls->base_access = row.access;
			model_.MutableInfo(info)->base_entity = row.cls->entity;
			// Base members become reachable through unqualified and
			// qualified member lookup (10.2 over the base DAG).
			scope->class_base = row.cls->members;
		}
		else
			scope->class_extra_bases.push_back(row.cls->members);
		// 14.6.2p3 applies per base: only a dependent-spelled base is
		// invisible to the pattern's unqualified lookup.
		if (instantiating_ && b < base_specs.size() &&
		    BaseClauseIsDependent(base_specs[b]->name))
			scope->dependent_base_links.push_back(row.cls->members);
	}
	// 8.5.1p1: a class with bases is not an aggregate.
	cls->is_aggregate = false;
	// PA18 14.6.2p3: a base spelled with a template parameter is
	// dependent; the instantiated scope remembers it so unqualified
	// lookup skips the base subtree.
	if (instantiating_ && any_dependent)
		scope->base_dependent = true;
	// PA17: the derived class inherits the primary base's vtable slots
	// (an overrider replaces in place) and the virtual-destructor
	// facts. Other polymorphic bases keep their own slot lists and
	// dispatch through their vtable views (PA27).
	if (cls->base && cls->base->is_polymorphic)
	{
		cls->vslots = cls->base->vslots;
		cls->dtor_slot = cls->base->dtor_slot;
	}
	BeginClassLayout(*cls);
}
