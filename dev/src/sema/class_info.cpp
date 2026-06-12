#include "sema/class_info.h"

#include "ast/ast.h"

namespace {

unsigned long long RoundUpBits(unsigned long long value,
                               unsigned long long alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

ClassInfo& ClassRegistry::Create(const NamedTypeInfo* entity)
{
	unique_ptr<ClassInfo>& slot = infos_[entity];
	if (!slot)
	{
		slot.reset(new ClassInfo());
		slot->entity = entity;
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
	if (info.has_user_dtor)
		return true;
	if (info.base && NeedsDestruction(*info.base))
		return true;
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		const ClassInfo* member = MemberClass(info.fields[i].type);
		if (member && NeedsDestruction(*member))
			return true;
	}
	return false;
}

bool ClassRegistry::NeedsConstruction(const ClassInfo& info) const
{
	if (info.has_user_ctor)
		return true;
	if (info.base && NeedsConstruction(*info.base))
		return true;
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		if (info.fields[i].default_init)
			return true;
		const ClassInfo* member = MemberClass(info.fields[i].type);
		if (member && NeedsConstruction(*member))
			return true;
	}
	return false;
}

// An analyzed special-member definition with no statements (and, for
// constructors, no mem-initializers) does nothing of its own.
static bool EmptyMemberBody(const AstStmt* body)
{
	return body && body->kind == SK_COMPOUND && body->items.empty();
}

bool ClassRegistry::DestructionHasEffects(const ClassInfo& info) const
{
	if (info.has_user_dtor &&
	    (!info.dtor_definition ||
	     !EmptyMemberBody(info.dtor_definition->body.get())))
		return true;
	if (info.base && DestructionHasEffects(*info.base))
		return true;
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		const ClassInfo* member = MemberClass(info.fields[i].type);
		if (member && DestructionHasEffects(*member))
			return true;
	}
	return false;
}

bool ClassRegistry::DefaultConstructionHasEffects(const ClassInfo& info) const
{
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
			// definition may do anything.
			if (!found->type->parameters.empty())
				return true;
			if (!found->definition ||
			    !found->definition->mem_initializers.empty() ||
			    !EmptyMemberBody(found->definition->body.get()))
				return true;
		}
	}
	if (info.base && DefaultConstructionHasEffects(*info.base))
		return true;
	for (size_t i = 0; i < info.fields.size(); i++)
	{
		if (info.fields[i].default_init)
			return true;
		const ClassInfo* member = MemberClass(info.fields[i].type);
		if (member && DefaultConstructionHasEffects(*member))
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
	if (!info.base)
		return;
	info.alignment = info.base->alignment;
	// The direct base subobject sits at offset 0; an empty base
	// occupies no storage (the empty-base optimization), every other
	// base reserves its full size before the first member.
	if (!info.base->is_empty)
	{
		info.bit_cursor = info.base->size * 8;
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

void FinishClassLayout(ClassInfo& info, NamedTypeInfo& entity)
{
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
		if (ctor.kind == kind && !ctor.implicit && !ctor.defaulted &&
		    !ctor.deleted)
			return true;
	}
	return false;
}

// Triviality of one subobject tree under a per-class predicate.
template <bool (*Predicate)(const ClassInfo&)>
bool SubobjectsSatisfy(const ClassInfo& info)
{
	if (info.base && !Predicate(*info.base))
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
	if (info.has_user_dtor || info.dtor_deleted)
		return false;
	return SubobjectsSatisfy<ClassHasTrivialDtor>(info);
}

bool ClassHasTrivialCopyCtor(const ClassInfo& info)
{
	if (UserProvidedCtor(info, CK_COPY))
		return false;
	return SubobjectsSatisfy<ClassHasTrivialCopyCtor>(info);
}

bool ClassHasTrivialMoveCtor(const ClassInfo& info)
{
	if (UserProvidedCtor(info, CK_MOVE))
		return false;
	return SubobjectsSatisfy<ClassHasTrivialMoveCtor>(info);
}

bool ClassHasTrivialCopyAssign(const ClassInfo& info)
{
	if (info.has_user_copy_assign)
		return false;
	return SubobjectsSatisfy<ClassHasTrivialCopyAssign>(info);
}

bool ClassHasTrivialMoveAssign(const ClassInfo& info)
{
	if (info.has_user_move_assign)
		return false;
	return SubobjectsSatisfy<ClassHasTrivialMoveAssign>(info);
}

bool ClassTriviallyCopyable(const ClassInfo& info)
{
	return ClassHasTrivialCopyCtor(info) && ClassHasTrivialMoveCtor(info) &&
		ClassHasTrivialCopyAssign(info) && ClassHasTrivialMoveAssign(info) &&
		ClassHasTrivialDtor(info);
}

bool ClassParamDirect(const ClassInfo& info)
{
	return info.size <= 16 && !info.is_union &&
		ClassHasTrivialMoveCtor(info) && ClassHasTrivialDtor(info);
}

bool ClassReturnDirect(const ClassInfo& info)
{
	return ClassParamDirect(info) && ClassHasTrivialCopyCtor(info);
}
