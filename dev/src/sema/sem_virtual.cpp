#include "sema/sem_binder.h"

#include <stdexcept>

using std::runtime_error;

// PA17 virtual-member machinery: vtable slot recording at declaration
// time, override/final/covariant checking (10.3), key-function
// selection, and the vpointer-store actions the constructor/destructor
// synthesis attaches to lowered bodies.

namespace {

// 10.3p2: an overrider matches by name, parameter-type-list, and
// method cv-qualification. `declared` is the unadjusted member type;
// the slot stores the this-adjusted type of its final overrider.
bool SlotSignatureMatches(const VirtualSlot& slot, const string& name,
                          const TypePtr& declared)
{
	if (slot.kind != VS_METHOD || slot.name != name)
		return false;
	const TypePtr& adjusted = slot.type;
	if (adjusted->parameters.size() != declared->parameters.size() + 1)
		return false;
	for (size_t i = 0; i < declared->parameters.size(); i++)
		if (!TypeEquals(adjusted->parameters[i + 1],
		                declared->parameters[i]))
			return false;
	const TypePtr& this_class = adjusted->parameters[0]->target;
	return this_class->is_const == declared->is_const &&
		this_class->is_volatile == declared->is_volatile;
}

// 10.3p7: identical return types, or covariant pointers/references to
// class where the overridden return class is a (possibly indirect)
// base of the overriding one with no less cv-qualification.
void CheckCovariantReturn(const TypePtr& overridden,
                          const TypePtr& overriding)
{
	if (TypeEquals(overridden, overriding))
		return;
	if (overridden->kind != overriding->kind ||
	    (overridden->kind != TK_POINTER &&
	     overridden->kind != TK_LVALUE_REFERENCE &&
	     overridden->kind != TK_RVALUE_REFERENCE))
		throw runtime_error("return type of the overriding function "
		                    "does not match the overridden virtual "
		                    "function");
	TypePtr base_target = overridden->target;
	TypePtr derived_target = overriding->target;
	if (RemoveTopCv(base_target)->kind != TK_CLASS ||
	    RemoveTopCv(derived_target)->kind != TK_CLASS ||
	    BaseClassDistance(RemoveTopCv(derived_target)->named,
	                      RemoveTopCv(base_target)->named) < 0 ||
	    (derived_target->is_const && !base_target->is_const) ||
	    (derived_target->is_volatile && !base_target->is_volatile))
		throw runtime_error("invalid covariant return type");
}

// Whether one direct member declaration spells the `virtual` keyword.
bool MemberSpellsVirtual(const AstDecl& member)
{
	switch (member.kind)
	{
	case DK_SIMPLE:
	case DK_FUNCTION:
		for (size_t i = 0; i < member.specifiers.size(); i++)
			if (member.specifiers[i].kind == SPEC_KEYWORD &&
			    member.specifiers[i].keyword == KW_VIRTUAL)
				return true;
		return false;
	case DK_SPECIAL_MEMBER_DECLARATION:
	case DK_SPECIAL_MEMBER_DEFINITION:
		for (size_t i = 0; i < member.member_specifiers.size(); i++)
			if (member.member_specifiers[i].keyword == KW_VIRTUAL)
				return true;
		return false;
	default:
		return false;
	}
}

}  // namespace

int FindVirtualSlotIndex(const ClassInfo& info, const string& name,
                         const TypePtr& declared)
{
	for (size_t i = 0; i < info.vslots.size(); i++)
		if (SlotSignatureMatches(info.vslots[i], name, declared))
			return (int)i;
	return -1;
}

// The layout pre-scan: whether the class body textually declares a
// virtual member (10.3p1 requires the keyword to introduce a new
// virtual function; overriding without it needs a polymorphic base,
// which carries the vpointer already). Nested class bodies scan for
// themselves when they open.
bool ClassBodyDeclaresVirtual(const AstDecl& decl)
{
	for (size_t i = 0; i < decl.members.size(); i++)
		if (MemberSpellsVirtual(*decl.members[i]))
			return true;
	return false;
}

void SemBinder::RecordVirtualMember(ClassInfo& cls, const string& name,
                                    const TypePtr& type,
                                    const DeclSpecifierInfo* specs,
                                    const DeclaratorInfo* composed,
                                    bool pure, bool defined)
{
	bool declared_virtual = specs && specs->is_virtual;
	bool has_override = composed && composed->has_override;
	bool has_final = composed && composed->has_final;
	bool is_static = specs && specs->is_static;
	if (is_static)
	{
		if (declared_virtual || has_override || has_final || pure)
			throw runtime_error("static member function declared "
			                    "virtual");
		return;
	}
	int slot = FindVirtualSlotIndex(cls, name, type);
	bool is_virtual = declared_virtual || slot >= 0;
	if (!is_virtual)
	{
		if (has_override)
			throw runtime_error("override specifier without an "
			                    "overridden virtual function");
		if (has_final)
			throw runtime_error("final specifier on a non-virtual "
			                    "member function");
		if (pure)
			throw runtime_error("pure-specifier on a non-virtual "
			                    "member function");
		return;
	}
	if (slot >= 0)
	{
		VirtualSlot& overridden = cls.vslots[slot];
		if (overridden.owner != cls.members)
		{
			// An override of an inherited virtual keeps the slot.
			if (overridden.is_final)
				throw runtime_error("overriding a final virtual "
				                    "function");
			CheckCovariantReturn(overridden.type->target, type->target);
			overridden.owner = cls.members;
			overridden.type = MethodAdjustedType(cls, type);
			overridden.pure = pure;
		}
		if (has_final)
			overridden.is_final = true;
	}
	else
	{
		if (has_override)
			throw runtime_error("override specifier without an "
			                    "overridden virtual function");
		VirtualSlot added;
		added.kind = VS_METHOD;
		added.name = name;
		added.type = MethodAdjustedType(cls, type);
		added.owner = cls.members;
		added.pure = pure;
		added.is_final = has_final;
		cls.vslots.push_back(added);
	}
	cls.is_aggregate = false;  // 8.5.1p1: no virtual functions
	// Itanium-style key function: the first declared non-pure virtual
	// member without an in-class definition anchors the vtable.
	if (!pure && !defined && cls.key_name.empty())
	{
		cls.key_name = name;
		cls.key_type = type;
		cls.key_is_dtor = false;
	}
}

void SemBinder::RecordVirtualDtor(ClassInfo& cls, bool declared_virtual,
                                  const DeclaratorInfo& composed,
                                  bool defined, bool defaulted,
                                  bool deleted)
{
	bool inherited = cls.base && cls.base->dtor_virtual;
	if (composed.has_override && !inherited)
		throw runtime_error("override specifier on a destructor "
		                    "without a virtual base destructor");
	if (inherited)
	{
		if (cls.dtor_slot >= 0 && cls.vslots[cls.dtor_slot].is_final)
			throw runtime_error("overriding a final virtual destructor");
		cls.dtor_virtual = true;
	}
	else if (declared_virtual)
	{
		cls.dtor_virtual = true;
		cls.dtor_slot = (int)cls.vslots.size();
		VirtualSlot complete;
		complete.kind = VS_DTOR_COMPLETE;
		complete.name = "~" + cls.members->name;
		complete.owner = cls.members;
		cls.vslots.push_back(complete);
		VirtualSlot deleting = complete;
		deleting.kind = VS_DTOR_DELETING;
		cls.vslots.push_back(deleting);
	}
	else if (composed.has_final)
		throw runtime_error("final specifier on a non-virtual "
		                    "destructor");
	if (composed.has_final && cls.dtor_slot >= 0)
	{
		cls.vslots[cls.dtor_slot].is_final = true;
		cls.vslots[cls.dtor_slot + 1].is_final = true;
	}
	if (cls.dtor_virtual && !defined && !defaulted && !deleted &&
	    cls.key_name.empty())
	{
		cls.key_name = "~" + cls.members->name;
		cls.key_is_dtor = true;
	}
}

void SemBinder::FinishClassVirtualFacts(ClassInfo& cls)
{
	// 12.4p9: the (possibly implicit) destructor of a class whose base
	// has a virtual destructor is virtual and overrides it.
	if (cls.base && cls.base->dtor_virtual)
		cls.dtor_virtual = true;
	if (!cls.dtor_virtual || cls.dtor_slot < 0)
		return;
	// The final overrider of the destructor slot pair is this class's
	// own destructor, user-declared or implicit.
	string name = "~" + cls.members->name;
	TypePtr adjusted = MethodAdjustedType(
		cls, MakeFunctionType(MakeFundamentalType(FT_VOID),
		                      vector<TypePtr>(), false));
	for (int at = cls.dtor_slot; at <= cls.dtor_slot + 1; at++)
	{
		cls.vslots[at].name = name;
		cls.vslots[at].owner = cls.members;
		cls.vslots[at].type = adjusted;
	}
}

SemNodePtr SemBinder::MakeVPointerStore(const ClassInfo& cls)
{
	SemNodePtr node = MakeSemNode(SN_VPOINTER_STORE);
	node->type = MakeNamedType(TK_CLASS, cls.entity);
	node->children.push_back(AddressOfNode(ThisObjectExpr()));
	return node;
}
