#include "sema/class_info.h"

#include <map>
#include <set>

#include "ast/ast.h"

namespace {

unsigned long long RoundUpBits(unsigned long long value,
                               unsigned long long alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

// One bit per memoized recursive class fact (ClassInfo::facts_*).
enum EClassFact
{
	CF_NEEDS_DESTRUCTION,
	CF_NEEDS_CONSTRUCTION,
	CF_DESTRUCTION_EFFECTS,
	CF_DEFAULT_CTOR_EFFECTS,
	CF_TRIVIAL_DTOR,
	CF_TRIVIAL_COPY_CTOR,
	CF_TRIVIAL_MOVE_CTOR,
	CF_TRIVIAL_COPY_ASSIGN,
	CF_TRIVIAL_MOVE_ASSIGN,
	CF_DEFAULT_CTOR_EFFECTS_SYNTAX
};

unsigned long long g_class_facts_version = 1;

bool FactCached(const ClassInfo& info, unsigned fact, bool& value)
{
	if (info.facts_version != g_class_facts_version)
	{
		info.facts_version = g_class_facts_version;
		info.facts_valid = 0;
	}
	if (!(info.facts_valid & (1u << fact)))
		return false;
	value = (info.facts_value >> fact) & 1u;
	return true;
}

bool FactStore(const ClassInfo& info, unsigned fact, bool value)
{
	info.facts_valid |= 1u << fact;
	if (value)
		info.facts_value |= 1u << fact;
	else
		info.facts_value &= ~(1u << fact);
	return value;
}

// The direct bases a subobject-fact walk traverses: the PA26 table
// when present, the primary link otherwise.
size_t DirectBaseCount(const ClassInfo& info)
{
	if (!info.direct_bases.empty())
		return info.direct_bases.size();
	return info.base ? 1 : 0;
}

const ClassInfo* DirectBaseAt(const ClassInfo& info, size_t index)
{
	return info.direct_bases.empty() ? info.base
	                                 : info.direct_bases[index].cls;
}

}  // namespace

void InvalidateClassFacts()
{
	g_class_facts_version++;
}

namespace {

// The visited sets keep shared subtrees (diamonds) linear; a visited
// class's chain and extras were already searched without finding `to`.
bool DerivedFromExtrasSearch(const ClassRegistry& classes,
                             const NamedTypeInfo* from,
                             const NamedTypeInfo* to,
                             std::set<const NamedTypeInfo*>& visited)
{
	for (const NamedTypeInfo* at = from; at; at = at->base_entity)
	{
		if (at == to)
			return true;
		if (!visited.insert(at).second)
			return false;
		if (const ClassInfo* info = classes.Find(at))
			for (size_t i = 1; i < info->direct_bases.size(); i++)
				if (DerivedFromExtrasSearch(
				        classes, info->direct_bases[i].cls->entity, to,
				        visited))
					return true;
	}
	return false;
}

bool DerivedFromLinkedSearch(const NamedTypeInfo* from,
                             const NamedTypeInfo* to,
                             std::set<const NamedTypeInfo*>& visited)
{
	for (const NamedTypeInfo* at = from; at; at = at->base_entity)
	{
		if (at == to)
			return true;
		if (!visited.insert(at).second)
			return false;
		if (at->class_record)
			for (size_t i = 1;
			     i < at->class_record->direct_bases.size(); i++)
				if (DerivedFromLinkedSearch(
				        at->class_record->direct_bases[i].cls->entity,
				        to, visited))
					return true;
	}
	return false;
}

}  // namespace

bool DerivedFromWithExtras(const ClassRegistry& classes,
                           const NamedTypeInfo* from,
                           const NamedTypeInfo* to)
{
	std::set<const NamedTypeInfo*> visited;
	return DerivedFromExtrasSearch(classes, from, to, visited);
}

bool FriendClassMatches(const NamedTypeInfo* granted,
                        const NamedTypeInfo* context)
{
	if (granted == context)
		return true;
	return granted && context && granted->is_template_anchor &&
		granted->spec_template &&
		context->spec_template == granted->spec_template;
}

bool DerivedFromWithExtrasLinked(const NamedTypeInfo* from,
                                 const NamedTypeInfo* to)
{
	std::set<const NamedTypeInfo*> visited;
	return DerivedFromLinkedSearch(from, to, visited);
}

namespace {

// The derivation-path facts of one class toward a fixed target: the
// path count (capped at 2 - one and "more than one" are the only
// distinctions 10.2 needs) and, for a unique path, its edge count and
// summed byte offset. Non-virtual bases only, so two paths always mean
// two distinct subobjects. Memoized per query so shared subtrees
// (diamond ladders) stay linear.
struct BasePathFact
{
	int count;
	int hops;
	unsigned long long offset;
};

BasePathFact CountBasePaths(const NamedTypeInfo* from,
                            const NamedTypeInfo* to,
                            std::map<const NamedTypeInfo*,
                                     BasePathFact>& memo)
{
	BasePathFact fact;
	fact.count = 0;
	fact.hops = 0;
	fact.offset = 0;
	if (from == to)
	{
		fact.count = 1;
		return fact;
	}
	if (!from)
		return fact;
	std::map<const NamedTypeInfo*, BasePathFact>::const_iterator seen =
		memo.find(from);
	if (seen != memo.end())
		return seen->second;
	const ClassInfo* record = from->class_record;
	if (record && !record->direct_bases.empty())
	{
		for (size_t i = 0; i < record->direct_bases.size(); i++)
		{
			const ClassDirectBase& row = record->direct_bases[i];
			BasePathFact sub = CountBasePaths(row.cls->entity, to, memo);
			if (!sub.count)
				continue;
			if (!fact.count)
			{
				fact.hops = sub.hops + 1;
				fact.offset = sub.offset + row.offset;
			}
			fact.count = fact.count + sub.count > 2
				? 2 : fact.count + sub.count;
		}
	}
	// Entities without a completed record (or whose record predates the
	// base table) fall back to the primary chain at offset 0.
	else if (from->base_entity)
	{
		BasePathFact sub = CountBasePaths(from->base_entity, to, memo);
		if (sub.count)
		{
			fact.count = sub.count;
			fact.hops = sub.hops + 1;
			fact.offset = sub.offset;
		}
	}
	memo[from] = fact;
	return fact;
}

}  // namespace

bool BaseAccessPath(const NamedTypeInfo* from, const NamedTypeInfo* to,
                    vector<ClassBaseEdge>& edges)
{
	edges.clear();
	if (!from || !to || from == to)
		return false;
	std::map<const NamedTypeInfo*, BasePathFact> memo;
	if (CountBasePaths(from, to, memo).count != 1)
		return false;
	// A unique path means exactly one direct base reaches `to` at each
	// step, so the memoized counts guide the walk. Link-only segments
	// (no base table) contribute no edge and act as public.
	const NamedTypeInfo* at = from;
	while (at && at != to)
	{
		const ClassInfo* record = at->class_record;
		if (!record || record->direct_bases.empty())
		{
			at = at->base_entity;
			continue;
		}
		size_t next = record->direct_bases.size();
		for (size_t i = 0; i < record->direct_bases.size(); i++)
			if (CountBasePaths(record->direct_bases[i].cls->entity, to,
			                   memo).count)
			{
				next = i;
				break;
			}
		if (next == record->direct_bases.size())
			return false;
		ClassBaseEdge edge;
		edge.derived = record;
		edge.base_index = next;
		edges.push_back(edge);
		at = record->direct_bases[next].cls->entity;
	}
	return at == to;
}

EBasePath BaseSubobjectPath(const NamedTypeInfo* from,
                            const NamedTypeInfo* to,
                            int& hops, unsigned long long& offset)
{
	hops = 0;
	offset = 0;
	if (!from || !to)
		return BP_NONE;
	if (from == to)
		return BP_UNIQUE;
	std::map<const NamedTypeInfo*, BasePathFact> memo;
	BasePathFact fact = CountBasePaths(from, to, memo);
	if (!fact.count)
		return BP_NONE;
	if (fact.count > 1)
		return BP_AMBIGUOUS;
	hops = fact.hops;
	offset = fact.offset;
	return BP_UNIQUE;
}

// The hop count of the unique derivation path (0 when `from == to`),
// or -1 when `to` is not an unambiguous base of `from`.
int BaseClassDistance(const NamedTypeInfo* from, const NamedTypeInfo* to)
{
	int hops = 0;
	unsigned long long offset = 0;
	return BaseSubobjectPath(from, to, hops, offset) == BP_UNIQUE
		? hops : -1;
}

bool ClassHasDisplacedBase(const ClassInfo* cls)
{
	if (!cls)
		return false;
	if (cls->direct_bases.empty())
		return ClassHasDisplacedBase(cls->base);
	for (size_t i = 0; i < cls->direct_bases.size(); i++)
	{
		if (cls->direct_bases[i].offset != 0)
			return true;
		if (ClassHasDisplacedBase(cls->direct_bases[i].cls))
			return true;
	}
	return false;
}

void CollectClassAndBases(const ClassInfo* cls,
                          vector<const ClassInfo*>& out)
{
	if (!cls)
		return;
	for (size_t i = 0; i < out.size(); i++)
		if (out[i] == cls)
			return;
	out.push_back(cls);
	if (cls->direct_bases.empty())
	{
		CollectClassAndBases(cls->base, out);
		return;
	}
	for (size_t i = 0; i < cls->direct_bases.size(); i++)
		CollectClassAndBases(cls->direct_bases[i].cls, out);
}

ClassInfo& ClassRegistry::Create(const NamedTypeInfo* entity)
{
	unique_ptr<ClassInfo>& slot = infos_[entity];
	if (!slot)
	{
		slot.reset(new ClassInfo());
		slot->entity = entity;
		order_.push_back(slot.get());
	}
	return *slot;
}

ClassInfo* ClassRegistry::Find(const NamedTypeInfo* entity)
{
	map<const NamedTypeInfo*, unique_ptr<ClassInfo>>::iterator found =
		infos_.find(entity);
	return found == infos_.end() ? 0 : found->second.get();
}

const ClassInfo* ClassRegistry::Find(const NamedTypeInfo* entity) const
{
	map<const NamedTypeInfo*, unique_ptr<ClassInfo>>::const_iterator found =
		infos_.find(entity);
	return found == infos_.end() ? 0 : found->second.get();
}

const ClassInfo* ClassRegistry::MemberClass(const TypePtr& type) const
{
	TypePtr inner = type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	if (inner->kind != TK_CLASS)
		return 0;
	return Find(inner->named);
}

bool ClassRegistry::NeedsDestruction(const ClassInfo& info) const
{
	bool value;
	if (FactCached(info, CF_NEEDS_DESTRUCTION, value))
		return value;
	value = info.has_user_dtor;
	for (size_t i = 0; !value && i < DirectBaseCount(info); i++)
		value = NeedsDestruction(*DirectBaseAt(info, i));
	for (size_t i = 0; !value && i < info.fields.size(); i++)
	{
		const ClassInfo* member = MemberClass(info.fields[i].type);
		value = member && NeedsDestruction(*member);
	}
	return FactStore(info, CF_NEEDS_DESTRUCTION, value);
}

bool ClassRegistry::NeedsConstruction(const ClassInfo& info) const
{
	bool value;
	if (FactCached(info, CF_NEEDS_CONSTRUCTION, value))
		return value;
	// PA17: constructing a polymorphic object must store the vpointer.
	value = info.has_user_ctor || info.is_polymorphic;
	for (size_t i = 0; !value && i < DirectBaseCount(info); i++)
		value = NeedsConstruction(*DirectBaseAt(info, i));
	for (size_t i = 0; !value && i < info.fields.size(); i++)
	{
		const ClassInfo* member = MemberClass(info.fields[i].type);
		value = info.fields[i].default_init ||
			(member && NeedsConstruction(*member));
	}
	return FactStore(info, CF_NEEDS_CONSTRUCTION, value);
}

// An analyzed special-member definition with no statements (and, for
// constructors, no mem-initializers) does nothing of its own.
static bool EmptyMemberBody(const AstStmt* body)
{
	return body && body->kind == SK_COMPOUND && body->items.empty();
}

bool ClassRegistry::DestructionHasEffects(const ClassInfo& info) const
{
	bool value;
	if (FactCached(info, CF_DESTRUCTION_EFFECTS, value))
		return value;
	// PA17: a polymorphic destructor re-stores the vpointer.
	value = info.is_polymorphic ||
		(info.has_user_dtor &&
		 (!info.dtor_definition ||
		  !EmptyMemberBody(info.dtor_definition->body.get())));
	for (size_t i = 0; !value && i < DirectBaseCount(info); i++)
		value = DestructionHasEffects(*DirectBaseAt(info, i));
	for (size_t i = 0; !value && i < info.fields.size(); i++)
	{
		const ClassInfo* member = MemberClass(info.fields[i].type);
		value = member && DestructionHasEffects(*member);
	}
	return FactStore(info, CF_DESTRUCTION_EFFECTS, value);
}

// PA18: whether the entity is (or is nested inside) an instantiated
// class-template specialization.
static bool EntityInInstantiation(const NamedTypeInfo* entity)
{
	if (!entity)
		return false;
	if (entity->spec_template && !entity->is_template_anchor)
		return true;
	for (const Scope* scope = entity->scope; scope;
	     scope = scope->parent)
		if (scope->entity && scope->entity->spec_template &&
		    !scope->entity->is_template_anchor)
			return true;
	return false;
}

bool ClassRegistry::DefaultConstructionHasEffects(const ClassInfo& info) const
{
	bool value;
	if (FactCached(info, CF_DEFAULT_CTOR_EFFECTS, value))
		return value;
	return FactStore(info, CF_DEFAULT_CTOR_EFFECTS,
	                 ComputeDefaultConstructionEffects(info, false));
}

// The order-independent variant: an instantiated user constructor
// reads its captured pattern body instead of staying conservative.
// Whole-object elision decisions (a local `S s;` through the implicit
// chain) run where the pattern is available; member-initialization
// inside synthesized bodies keeps the conservative fact above (the
// reference pins the member call there).
bool ClassRegistry::DefaultConstructionHasSyntacticEffects(
	const ClassInfo& info) const
{
	bool value;
	if (FactCached(info, CF_DEFAULT_CTOR_EFFECTS_SYNTAX, value))
		return value;
	return FactStore(info, CF_DEFAULT_CTOR_EFFECTS_SYNTAX,
	                 ComputeDefaultConstructionEffects(info, true));
}

bool ClassRegistry::ComputeDefaultConstructionEffects(
	const ClassInfo& info, bool syntactic) const
{
	// PA17: the vpointer store is emitted construction work.
	if (info.is_polymorphic)
		return true;
	if (info.has_user_ctor)
	{
		const ClassCtor* found = 0;
		for (size_t i = 0; i < info.ctors.size(); i++)
		{
			const ClassCtor& ctor = info.ctors[i];
			if (ctor.inherited_base)
				continue;
			size_t required = ctor.type->parameters.size();
			while (required > 0 && required <= ctor.defaults.size() &&
			       ctor.defaults[required - 1])
				required--;
			if (required == 0)
			{
				found = &ctor;
				break;
			}
		}
		if (!found)
			return true;
		if (!found->defaulted)
		{
			// Default arguments evaluate per call; an unseen
			// definition may do anything. PA18: an instantiated user
			// constructor keeps its call (the reference outputs pin
			// point-of-instantiation ordering, where the body is not
			// yet analyzed when the enclosing constructor lowers)
			// unless the caller asked for the syntactic walk. PA25:
			// the syntactic walk elides only instantiated pattern
			// bodies; a plain user-provided constructor keeps its
			// call in the chain.
			if (EntityInInstantiation(info.entity))
			{
				if (!syntactic)
					return true;
			}
			else if (syntactic)
				return true;
			if (!found->type->parameters.empty())
				return true;
			if (!found->definition ||
			    !found->definition->mem_initializers.empty() ||
			    !EmptyMemberBody(found->definition->body.get()))
				return true;
		}
	}
	for (size_t i = 0; i < DirectBaseCount(info); i++)
	{
		const ClassInfo& base = *DirectBaseAt(info, i);
		if (syntactic ? DefaultConstructionHasSyntacticEffects(base)
		              : DefaultConstructionHasEffects(base))
			return true;
	}
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		if (info.fields[i].default_init)
			return true;
		const ClassInfo* member = MemberClass(info.fields[i].type);
		if (member &&
		    (syntactic ? DefaultConstructionHasSyntacticEffects(*member)
		               : DefaultConstructionHasEffects(*member)))
			return true;
	}
	return false;
}

// --- layout -----------------------------------------------------------

void BeginClassLayout(ClassInfo& info)
{
	info.bit_cursor = 0;
	info.alignment = 1;
	info.is_empty = true;
	info.is_polymorphic = info.declares_virtual ||
		(info.base && info.base->is_polymorphic);
	// PA26: direct bases lay out in declaration order. An empty base
	// occupies no storage and stays at offset 0 (the empty-base
	// optimization; distinct empty subobjects share the offset in this
	// ABI), every other base reserves its span before the next base or
	// the first member.
	for (size_t i = 0; i < info.direct_bases.size(); i++)
	{
		ClassDirectBase& row = info.direct_bases[i];
		if (row.cls->alignment > info.alignment)
			info.alignment = row.cls->alignment;
		if (row.cls->is_empty)
		{
			row.offset = 0;
			continue;
		}
		row.offset = RoundUpBits(info.bit_cursor,
		                         row.cls->alignment * 8) / 8;
		info.bit_cursor = (row.offset + row.cls->size) * 8;
		info.is_empty = false;
	}
	// PA17: a class that introduces the polymorphic layer carries the
	// vpointer at offset 0 ahead of every field. A polymorphic base
	// already reserved it inside its own span.
	if (info.is_polymorphic && (!info.base || !info.base->is_polymorphic))
	{
		if (info.bit_cursor < 64)
			info.bit_cursor = 64;
		if (info.alignment < 8)
			info.alignment = 8;
		info.is_empty = false;
	}
}

ClassField& LayoutField(ClassInfo& info, const ClassField& field)
{
	unsigned long long field_size = TypeSize(field.type);
	unsigned long long field_alignment = TypeAlignment(field.type);
	ClassField row = field;
	row.is_bit_field = false;
	if (info.is_union)
	{
		row.offset = 0;
		if (field_size * 8 > info.bit_cursor)
			info.bit_cursor = field_size * 8;
	}
	else
	{
		unsigned long long bytes = (info.bit_cursor + 7) / 8;
		row.offset = RoundUpBits(bytes, field_alignment);
		info.bit_cursor = (row.offset + field_size) * 8;
	}
	if (field_alignment > info.alignment)
		info.alignment = field_alignment;
	info.is_empty = false;
	info.fields.push_back(row);
	return info.fields.back();
}

ClassField& LayoutBitField(ClassInfo& info, const ClassField& field)
{
	unsigned long long unit_bytes = TypeSize(field.type);
	unsigned long long unit_bits = unit_bytes * 8;
	unsigned long long align_bits = TypeAlignment(field.type) * 8;
	ClassField row = field;
	row.is_bit_field = true;
	if (row.bit_width == 0)
	{
		// 9.6p2: a zero-width unnamed bit-field realigns the cursor to
		// the next unit boundary without allocating anything.
		info.bit_cursor = RoundUpBits(info.bit_cursor, align_bits);
		row.offset = info.bit_cursor / 8;
		row.bit_offset = 0;
		info.fields.push_back(row);
		return info.fields.back();
	}
	unsigned long long start = info.is_union ? 0 : info.bit_cursor;
	// Itanium allocation: the field packs into the next aligned unit
	// of its declared type that can hold the whole width.
	if (start / unit_bits != (start + row.bit_width - 1) / unit_bits)
		start = RoundUpBits(start, unit_bits);
	row.offset = start / unit_bits * unit_bytes;
	row.bit_offset = start - row.offset * 8;
	unsigned long long end = start + row.bit_width;
	if (end > info.bit_cursor || info.is_union)
	{
		if (info.is_union)
		{
			if (end > info.bit_cursor)
				info.bit_cursor = end;
		}
		else
			info.bit_cursor = end;
	}
	// Unnamed bit-fields are not members; their type does not affect
	// the aggregate's alignment (9.6p2 with the x86-64 ABI).
	if (!row.name.empty() && align_bits / 8 > info.alignment)
		info.alignment = align_bits / 8;
	info.is_empty = false;
	info.fields.push_back(row);
	return info.fields.back();
}

void FinishClassLayout(ClassInfo& info, NamedTypeInfo& entity,
                       unsigned long long min_alignment)
{
	if (min_alignment > info.alignment)
		info.alignment = min_alignment;
	info.dsize = (info.bit_cursor + 7) / 8;
	unsigned long long size =
		RoundUpBits(info.dsize, info.alignment);
	info.size = size ? size : 1;
	entity.size = info.size;
	entity.alignment = info.alignment;
}

// --- queries ----------------------------------------------------------

const ClassField* FindClassField(const ClassInfo& info, const string& name)
{
	if (name.empty())
		return 0;
	for (size_t i = 0; i < info.fields.size(); i++)
		if (info.fields[i].name == name)
			return &info.fields[i];
	return 0;
}

int ClassCtorIndex(const ClassInfo& info, const TypePtr& type)
{
	for (size_t i = 0; i < info.ctors.size(); i++)
		if (TypeEquals(info.ctors[i].type, type))
			return (int)i;
	return -1;
}

ECtorKind ClassifyCtorKind(const NamedTypeInfo* entity,
                           const ClassCtor& ctor)
{
	const vector<TypePtr>& params = ctor.type->parameters;
	if (params.empty())
		return CK_ORDINARY;
	for (size_t i = 1; i < params.size(); i++)
		if (i >= ctor.defaults.size() || !ctor.defaults[i])
			return CK_ORDINARY;
	const TypePtr& first = params[0];
	if (!IsReferenceType(first))
		return CK_ORDINARY;
	TypePtr referee = RemoveTopCv(first->target);
	if (referee->kind != TK_CLASS || referee->named != entity)
		return CK_ORDINARY;
	return first->kind == TK_LVALUE_REFERENCE ? CK_COPY : CK_MOVE;
}

// --- PA16 triviality facts (9p6, 12.8) --------------------------------

const ClassInfo* SubobjectClass(const TypePtr& type)
{
	TypePtr inner = type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	inner = RemoveTopCv(inner);
	if (inner->kind != TK_CLASS)
		return 0;
	return inner->named->class_record;
}

namespace {

// Whether a user-provided (declared, not defaulted, not deleted)
// constructor of the given special kind exists.
bool UserProvidedCtor(const ClassInfo& info, ECtorKind kind)
{
	for (size_t i = 0; i < info.ctors.size(); i++)
	{
		const ClassCtor& ctor = info.ctors[i];
		if (ctor.kind != kind || ctor.implicit || ctor.deleted)
			continue;
		if (!ctor.defaulted || ctor.defaulted_outside)
			return true;
	}
	return false;
}

// Triviality of one subobject tree under a per-class predicate.
template <bool (*Predicate)(const ClassInfo&)>
bool SubobjectsSatisfy(const ClassInfo& info)
{
	for (size_t i = 0; i < DirectBaseCount(info); i++)
		if (!Predicate(*DirectBaseAt(info, i)))
			return false;
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		const ClassInfo* member = SubobjectClass(info.fields[i].type);
		if (member && !Predicate(*member))
			return false;
	}
	return true;
}

}  // namespace

bool ClassHasTrivialDtor(const ClassInfo& info)
{
	bool value;
	if (FactCached(info, CF_TRIVIAL_DTOR, value))
		return value;
	// 12.4p5: a virtual destructor is not trivial.
	value = !info.has_user_dtor && !info.dtor_deleted &&
		!info.dtor_virtual &&
		SubobjectsSatisfy<ClassHasTrivialDtor>(info);
	return FactStore(info, CF_TRIVIAL_DTOR, value);
}

bool ClassHasTrivialCopyCtor(const ClassInfo& info)
{
	bool value;
	if (FactCached(info, CF_TRIVIAL_COPY_CTOR, value))
		return value;
	// 12.8p12: virtual functions make the copy constructor non-trivial.
	value = !UserProvidedCtor(info, CK_COPY) && !info.is_polymorphic &&
		SubobjectsSatisfy<ClassHasTrivialCopyCtor>(info);
	return FactStore(info, CF_TRIVIAL_COPY_CTOR, value);
}

bool ClassHasTrivialMoveCtor(const ClassInfo& info)
{
	bool value;
	if (FactCached(info, CF_TRIVIAL_MOVE_CTOR, value))
		return value;
	value = !UserProvidedCtor(info, CK_MOVE) && !info.is_polymorphic &&
		SubobjectsSatisfy<ClassHasTrivialMoveCtor>(info);
	return FactStore(info, CF_TRIVIAL_MOVE_CTOR, value);
}

bool ClassHasTrivialCopyAssign(const ClassInfo& info)
{
	bool value;
	if (FactCached(info, CF_TRIVIAL_COPY_ASSIGN, value))
		return value;
	// 12.8p25: virtual functions make copy assignment non-trivial.
	value = !info.has_user_copy_assign && !info.is_polymorphic &&
		SubobjectsSatisfy<ClassHasTrivialCopyAssign>(info);
	return FactStore(info, CF_TRIVIAL_COPY_ASSIGN, value);
}

bool ClassHasTrivialMoveAssign(const ClassInfo& info)
{
	bool value;
	if (FactCached(info, CF_TRIVIAL_MOVE_ASSIGN, value))
		return value;
	value = !info.has_user_move_assign && !info.is_polymorphic &&
		SubobjectsSatisfy<ClassHasTrivialMoveAssign>(info);
	return FactStore(info, CF_TRIVIAL_MOVE_ASSIGN, value);
}

bool ClassTriviallyCopyable(const ClassInfo& info)
{
	return ClassHasTrivialCopyCtor(info) && ClassHasTrivialMoveCtor(info) &&
		ClassHasTrivialCopyAssign(info) && ClassHasTrivialMoveAssign(info) &&
		ClassHasTrivialDtor(info);
}

// The reference's trivial-for-the-purposes-of-calls walk: subobjects
// recurse, but a class's OWN user-provided copy/move constructor
// counts only when it has no base class (the pa16 direct-object
// passthrough pins the with-base form direct; the pa22
// conversion-target fixtures pin the base-less form by-address).
static bool ClassPassesDirect(const ClassInfo& info)
{
	if (info.is_polymorphic || info.is_union)
		return false;
	if (!info.base && (UserProvidedCtor(info, CK_COPY) ||
	                   UserProvidedCtor(info, CK_MOVE)))
		return false;
	return SubobjectsSatisfy<ClassPassesDirect>(info);
}

// Itanium call convention: a deleted copy constructor (a move-only
// class) makes the object non-trivial for calls; it passes and
// returns indirectly.
static bool ClassCopyCtorDeleted(const ClassInfo& info)
{
	for (size_t i = 0; i < info.ctors.size(); i++)
		if (info.ctors[i].kind == CK_COPY && info.ctors[i].deleted)
			return true;
	return false;
}

bool ClassParamDirect(const ClassInfo& info)
{
	return info.size <= 16 && !info.is_union &&
		ClassHasTrivialMoveCtor(info) && ClassHasTrivialDtor(info) &&
		!ClassCopyCtorDeleted(info) && ClassPassesDirect(info);
}

bool ClassReturnDirect(const ClassInfo& info)
{
	return ClassParamDirect(info) && ClassHasTrivialCopyCtor(info);
}
