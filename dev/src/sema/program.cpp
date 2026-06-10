#include "sema/program.h"

#include <stdexcept>

using std::runtime_error;

Program::Program() : tu_(-1)
{
}

void Program::BeginTranslationUnit()
{
	tu_++;
}

DeclaredEntity* Program::LinkEntity(const string& key, ESlotKind kind,
                                    const string& name, const TypePtr& type,
                                    ELinkage linkage, bool& created)
{
	if (!key.empty())
	{
		map<string, DeclaredEntity*>::iterator it = linked_.find(key);
		if (it != linked_.end())
		{
			created = false;
			return it->second;
		}
	}
	created = true;
	entities_.emplace_back(new DeclaredEntity());
	DeclaredEntity* entity = entities_.back().get();
	entity->kind = kind;
	entity->name = name;
	entity->type = type;
	entity->linkage = linkage;
	if (!key.empty())
		linked_[key] = entity;
	block1_.push_back(entity);
	return entity;
}

void Program::RecordDefinition(DeclaredEntity* entity)
{
	if (entity->kind == SLOT_FUNCTION)
	{
		if (entity->defined_tu >= 0 &&
		    (!entity->is_inline || entity->defined_tu == tu_))
			throw runtime_error("redefinition of function `" + entity->name +
			                    "`");
		entity->defined_tu = tu_;
		return;
	}
	if (entity->defined_tu >= 0)
		throw runtime_error("redefinition of variable `" + entity->name +
		                    "`");
	entity->defined_tu = tu_;
}

ImageSlot* Program::CreateTemporary(const TypePtr& type)
{
	objects_.emplace_back(new ImageSlot());
	ImageSlot* slot = objects_.back().get();
	slot->kind = SLOT_TEMPORARY;
	slot->type = type;
	slot->defined_tu = tu_;
	block2_.push_back(slot);
	return slot;
}

ImageSlot* Program::CreateStringLiteral(const PostToken& token)
{
	objects_.emplace_back(new ImageSlot());
	ImageSlot* slot = objects_.back().get();
	slot->kind = SLOT_STRING;
	slot->type = MakeArrayType(
		MakeCvQualifiedType(MakeFundamentalType(token.type), true, false),
		true, token.num_elements);
	slot->init_kind = INIT_BYTES;
	slot->init_bytes = token.data;
	slot->init_is_constant = true;
	slot->defined_tu = tu_;
	block3_.push_back(slot);
	return slot;
}

namespace {

// The handout's mock representation of every function stub.
const char kFunctionStub[4] = { 'f', 'u', 'n', '\0' };

unsigned long long AlignUp(unsigned long long offset,
                           unsigned long long alignment)
{
	unsigned long long misalign = offset % alignment;
	unsigned long long padding = misalign ? alignment - misalign : 0;
	if (offset + padding < offset)
		throw runtime_error("program image exceeds implementation limits");
	return offset + padding;
}

unsigned long long PlaceSlot(ImageSlot* slot, unsigned long long offset)
{
	unsigned long long size, alignment;
	if (slot->kind == SLOT_FUNCTION)
		size = alignment = 4;  // mock stub per the handout
	else
	{
		size = TypeSize(slot->type);
		alignment = TypeAlignment(slot->type);
	}
	offset = AlignUp(offset, alignment);
	slot->offset = offset;
	slot->placed = true;
	if (offset + size < offset)
		throw runtime_error("program image exceeds implementation limits");
	return offset + size;
}

void WriteSlot(string& image, const ImageSlot* slot)
{
	if (slot->kind == SLOT_FUNCTION)
	{
		image.replace(slot->offset, 4, kFunctionStub, 4);
		return;
	}
	switch (slot->init_kind)
	{
	case INIT_ZERO:
		break;
	case INIT_BYTES:
		image.replace(slot->offset, slot->init_bytes.size(),
		              slot->init_bytes);
		break;
	case INIT_ADDRESS:
	{
		const ImageSlot* target = slot->init_target;
		unsigned long long address =
			(target && target->placed ? target->offset : 0) +
			slot->init_addend;
		image.replace(slot->offset, 8, LittleEndianBytes(address, 8));
		break;
	}
	}
}

} // namespace

void Program::WriteImage(ostream& out) const
{
	unsigned long long offset = 4;  // the "PA8" magic
	vector<ImageSlot*> placed;
	for (size_t i = 0; i < block1_.size(); i++)
	{
		// Only defined variables appear in the image; functions need
		// only a declaration (they are fixed stubs).
		if (block1_[i]->kind == SLOT_VARIABLE && block1_[i]->defined_tu < 0)
			continue;
		offset = PlaceSlot(block1_[i], offset);
		placed.push_back(block1_[i]);
	}
	for (size_t i = 0; i < block2_.size(); i++)
	{
		offset = PlaceSlot(block2_[i], offset);
		placed.push_back(block2_[i]);
	}
	for (size_t i = 0; i < block3_.size(); i++)
	{
		offset = PlaceSlot(block3_[i], offset);
		placed.push_back(block3_[i]);
	}
	string image(offset, '\0');
	image.replace(0, 4, "PA8\0", 4);
	for (size_t i = 0; i < placed.size(); i++)
		WriteSlot(image, placed[i]);
	out.write(image.data(), image.size());
}
