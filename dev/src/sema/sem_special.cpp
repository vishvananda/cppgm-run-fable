#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA16 copy/move special members: the implicit declarations appended
// at class completion (12.8) and the demand-driven synthesis of
// copy/move constructor and assignment definitions, including the
// leading trivially-copyable storage-prefix `copyobj` form.

namespace {

// Whether any constructor of the given special kind is usable
// (declared and not deleted).
bool HasUsableCtor(const ClassInfo& cls, ECtorKind kind)
{
	for (size_t i = 0; i < cls.ctors.size(); i++)
		if (cls.ctors[i].kind == kind && !cls.ctors[i].deleted)
			return true;
	return false;
}

// 12.8p11: some base or member of class type cannot be copied.
bool SubobjectCopyUnavailable(const ClassInfo& cls)
{
	for (size_t i = 0; i < cls.direct_bases.size(); i++)
		if (!HasUsableCtor(*cls.direct_bases[i].cls, CK_COPY))
			return true;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassInfo* member = SubobjectClass(cls.fields[i].type);
		if (member && !HasUsableCtor(*member, CK_COPY))
			return true;
	}
	return false;
}

// 12.8p11: some base or member can be neither moved nor copied.
bool SubobjectMoveUnavailable(const ClassInfo& cls)
{
	for (size_t i = 0; i < cls.direct_bases.size(); i++)
	{
		const ClassInfo& base = *cls.direct_bases[i].cls;
		if (!HasUsableCtor(base, CK_MOVE) &&
		    !HasUsableCtor(base, CK_COPY))
			return true;
	}
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassInfo* member = SubobjectClass(cls.fields[i].type);
		if (member && !HasUsableCtor(*member, CK_MOVE) &&
		    !HasUsableCtor(*member, CK_COPY))
			return true;
	}
	return false;
}

// 9.5p2: a union's implicit copy/move members are deleted unless every
// variant member's corresponding member is trivial.
bool UnionMemberBlocks(const ClassInfo& cls,
                       bool (*trivial)(const ClassInfo&))
{
	if (!cls.is_union)
		return false;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassInfo* member = SubobjectClass(cls.fields[i].type);
		if (member && !trivial(*member))
			return true;
	}
	return false;
}

// Whether the class's copy assignment is usable (12.8p23 recursion).
bool HasUsableCopyAssign(const ClassInfo& cls)
{
	if (cls.has_user_copy_assign)
		return !cls.copy_assign_deleted;
	return cls.copy_assign_index >= 0 && !cls.copy_assign_deleted;
}

bool HasUsableMoveAssign(const ClassInfo& cls)
{
	if (cls.has_user_move_assign)
		return true;
	return cls.move_assign_index >= 0;
}

// 12.8p23: members that make an implicit copy/move assignment deleted.
bool AssignBlockedByMembers(const ClassInfo& cls, bool is_move)
{
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		if (IsReferenceType(field.type))
			return true;
		bool is_const = false;
		bool is_volatile = false;
		TopCv(field.type, is_const, is_volatile);
		if (is_const)
			return true;
		const ClassInfo* member = SubobjectClass(field.type);
		if (member && !HasUsableCopyAssign(*member) &&
		    !(is_move && HasUsableMoveAssign(*member)))
			return true;
	}
	for (size_t i = 0; i < cls.direct_bases.size(); i++)
	{
		const ClassInfo& base = *cls.direct_bases[i].cls;
		if (!HasUsableCopyAssign(base) &&
		    !(is_move && HasUsableMoveAssign(base)))
			return true;
	}
	return false;
}

// 12.8p8: the implicit copy constructor takes const C& iff every
// class subobject's copy constructor takes a const reference.
bool SubobjectConstCopyParam(const ClassInfo& member)
{
	bool any = false;
	for (size_t i = 0; i < member.ctors.size(); i++)
	{
		const ClassCtor& ctor = member.ctors[i];
		if (ctor.kind != CK_COPY)
			continue;
		any = true;
		const TypePtr& param = ctor.type->parameters[0];
		if (param->target->is_const)
			return true;
	}
	return !any;
}

bool SubobjectsConstCopy(const ClassInfo& cls)
{
	for (size_t i = 0; i < cls.direct_bases.size(); i++)
		if (!SubobjectConstCopyParam(*cls.direct_bases[i].cls))
			return false;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassInfo* member = SubobjectClass(cls.fields[i].type);
		if (member && !SubobjectConstCopyParam(*member))
			return false;
	}
	return true;
}

// The per-subobject triviality predicate of one transfer form. A
// subobject with a user-provided destructor transfers through its own
// special member (the reference keeps the member call visible instead
// of coalescing it into the storage prefix).
bool TransferTrivial(const ClassInfo& member, bool is_move, bool assign)
{
	if (member.has_user_dtor)
		return false;
	if (assign)
		return is_move ? ClassHasTrivialMoveAssign(member)
		               : ClassHasTrivialCopyAssign(member);
	return is_move ? ClassHasTrivialMoveCtor(member)
	               : ClassHasTrivialCopyCtor(member);
}

}  // namespace

// --- implicit declaration at class completion --------------------------------

void SemBinder::DeclareImplicitSpecialMembers(ClassInfo& cls)
{
	if (cls.specials_declared)
		return;
	cls.specials_declared = true;
	// Classify the user-declared assignment operators (12.8p17/p19).
	// Explicitly-defaulted ones synthesize like the implicit members.
	if (ScopeBinding* assign = FindOwnBinding(*cls.members, "operator ="))
	{
		// PA21: a template-only operator= binding has no ordinary
		// overload entries (12.8p17 last sentence: an assignment
		// operator template never declares a copy/move assignment).
		size_t count = assign->type ? assign->overloads.size() + 1 : 0;
		for (size_t i = 0; assign->kind == SB_FUNCTION && i < count; i++)
		{
			const TypePtr& declared =
				i == 0 ? assign->type : assign->overloads[i - 1];
			if (declared->parameters.size() != 1)
				continue;
			const TypePtr& param = declared->parameters[0];
			TypePtr bare = IsReferenceType(param) ? param->target : param;
			bare = RemoveTopCv(bare);
			if (bare->kind != TK_CLASS || bare->named != cls.entity)
				continue;
			bool is_move = param->kind == TK_RVALUE_REFERENCE;
			if (is_move)
				cls.has_user_move_assign = true;
			else
			{
				cls.has_user_copy_assign = true;
				if (i < assign->fn_deleted.size() &&
				    assign->fn_deleted[i])
					cls.copy_assign_deleted = true;
			}
			if (i < assign->fn_defaulted.size() &&
			    assign->fn_defaulted[i])
			{
				if (is_move)
					cls.move_assign_index = (int)i;
				else
					cls.copy_assign_index = (int)i;
				if (AssignBlockedByMembers(cls, is_move))
				{
					assign->fn_deleted.resize(count, false);
					assign->fn_deleted[i] = true;
					if (!is_move)
						cls.copy_assign_deleted = true;
				}
			}
		}
	}
	// A user-defaulted copy/move constructor may be defined as deleted
	// by its subobjects; a deleted defaulted move is ignored (12.8p9).
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		ClassCtor& ctor = cls.ctors[i];
		if (!ctor.defaulted || ctor.deleted)
			continue;
		if (ctor.kind == CK_COPY && (SubobjectCopyUnavailable(cls) ||
		    UnionMemberBlocks(cls, ClassHasTrivialCopyCtor)))
			ctor.deleted = true;
		else if (ctor.kind == CK_MOVE && (SubobjectMoveUnavailable(cls) ||
		         UnionMemberBlocks(cls, ClassHasTrivialMoveCtor)))
		{
			ctor.deleted = true;
			ctor.ignore_in_overload = true;
		}
	}
	bool suppress_moves = cls.has_user_copy_ctor || cls.has_user_move_ctor ||
		cls.has_user_copy_assign || cls.has_user_move_assign ||
		cls.dtor_user_declared;
	TypePtr class_type = MakeNamedType(TK_CLASS, cls.entity);
	TypePtr void_type = MakeFundamentalType(FT_VOID);
	// 12.8p7/p8: the implicitly declared copy constructor.
	if (!cls.has_user_copy_ctor)
	{
		TypePtr referee = SubobjectsConstCopy(cls)
			? MakeCvQualifiedType(class_type, true, false) : class_type;
		vector<TypePtr> params;
		params.push_back(MakeReferenceType(referee, false, true));
		ClassCtor copy;
		copy.type = MakeFunctionType(void_type, params, false);
		copy.kind = CK_COPY;
		copy.implicit = true;
		copy.param_names.push_back("");
		copy.defaults.push_back(0);
		copy.deleted = cls.has_user_move_ctor || cls.has_user_move_assign ||
			SubobjectCopyUnavailable(cls) ||
			UnionMemberBlocks(cls, ClassHasTrivialCopyCtor);
		cls.ctors.push_back(copy);
	}
	// 12.8p9: the implicitly declared move constructor (a deleted one
	// is ignored by overload resolution, so it is simply not appended).
	if (!suppress_moves && !SubobjectMoveUnavailable(cls) &&
	    !UnionMemberBlocks(cls, ClassHasTrivialMoveCtor))
	{
		vector<TypePtr> params;
		params.push_back(MakeReferenceType(class_type, true, true));
		ClassCtor move;
		move.type = MakeFunctionType(void_type, params, false);
		move.kind = CK_MOVE;
		move.implicit = true;
		move.param_names.push_back("");
		move.defaults.push_back(0);
		cls.ctors.push_back(move);
	}
	// 12.8p18/p23: the implicitly declared copy assignment operator.
	if (!cls.has_user_copy_assign)
	{
		bool deleted = cls.has_user_move_ctor || cls.has_user_move_assign ||
			AssignBlockedByMembers(cls, false) ||
			UnionMemberBlocks(cls, ClassHasTrivialCopyAssign);
		DeclareImplicitAssign(cls, false, deleted);
	}
	// 12.8p20: the implicitly declared move assignment operator (a
	// deleted one is ignored, so it is not declared).
	if (!suppress_moves && !AssignBlockedByMembers(cls, true) &&
	    !UnionMemberBlocks(cls, ClassHasTrivialMoveAssign))
		DeclareImplicitAssign(cls, true, false);
}

void SemBinder::DeclareImplicitAssign(ClassInfo& cls, bool is_move,
                                      bool deleted)
{
	TypePtr class_type = MakeNamedType(TK_CLASS, cls.entity);
	TypePtr param;
	if (is_move)
		param = MakeReferenceType(class_type, true, true);
	else
		param = MakeReferenceType(
			MakeCvQualifiedType(class_type, true, false), false, true);
	vector<TypePtr> params;
	params.push_back(param);
	TypePtr fn = MakeFunctionType(MakeReferenceType(class_type, false, true),
	                              params, false);
	ScopeBinding* binding = FindOwnBinding(*cls.members, "operator =");
	size_t index;
	if (!binding)
	{
		ScopeBinding fresh;
		fresh.kind = SB_FUNCTION;
		fresh.name = "operator =";
		fresh.type = fn;
		binding = &AddBinding(*cls.members, fresh);
		index = 0;
	}
	else if (!binding->type)
	{
		// PA21: a template-only operator= binding gains the implicit
		// member as its first ordinary overload entry.
		binding->type = fn;
		index = 0;
	}
	else
	{
		binding->overloads.push_back(fn);
		index = binding->overloads.size();
	}
	binding->fn_defaults.resize(index + 1);
	binding->fn_deleted.resize(index + 1, false);
	binding->fn_deleted[index] = deleted;
	binding->fn_access.resize(index + 1, MA_PUBLIC);
	binding->fn_access[index] = MA_PUBLIC;
	binding->fn_static.resize(index + 1, false);
	binding->fn_inline_def.resize(index + 1, false);
	binding->fn_adl_only.resize(index + 1, false);
	binding->fn_unwind_no.resize(index + 1, false);
	binding->fn_noexcept_decl.resize(index + 1, false);
	if (is_move)
		cls.move_assign_index = (int)index;
	else
	{
		cls.copy_assign_index = (int)index;
		cls.copy_assign_deleted = deleted;
	}
}

// --- synthesized bodies -------------------------------------------------------

SemNodePtr SemBinder::SourceObjectExpr(Scope* fn_scope, const string& name,
                                       const TypePtr& class_type)
{
	SemNodePtr object = MakeSemNode(SN_ID_EXPRESSION);
	object->name = name;
	object->type = class_type;
	object->category = VC_LVALUE;
	object->entity_scope = fn_scope;
	object->entity_name = name;
	return object;
}

SemNodePtr SemBinder::SourceFieldExpr(const SemNode& source_proto,
                                      const ClassField& field,
                                      EValueCategory category)
{
	SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
	member->name = field.name;
	member->type = IsReferenceType(field.type) ? field.type->target
	                                           : field.type;
	member->member_ref = IsReferenceType(field.type);
	member->category = category == VC_PRVALUE ? VC_LVALUE : category;
	member->member_offset = field.offset;
	member->is_bit_field = field.is_bit_field;
	member->bit_offset = field.bit_offset;
	member->bit_width = field.bit_width;
	member->children.push_back(CloneSemNode(source_proto));
	return member;
}

SemNodePtr SemBinder::StorageCopyAction(const ClassInfo& cls,
                                        const SemNode& source_proto,
                                        unsigned long long span,
                                        unsigned long long alignment)
{
	(void)cls;
	SemNodePtr action = MakeSemNode(SN_STORAGE_COPY);
	action->has_value = true;
	action->value = ConstValue(FT_UNSIGNED_LONG_INT, span);
	action->bit_width = alignment;
	action->children.push_back(ThisObjectExpr());
	action->children.push_back(CloneSemNode(source_proto));
	return action;
}

unsigned long long SemBinder::TrivialStoragePrefix(
	const ClassInfo& cls, bool is_move, bool assign_form,
	unsigned long long& alignment, size_t& first_suffix)
{
	alignment = 1;
	first_suffix = 0;
	// PA17: a polymorphic object's storage prefix starts with the
	// vpointer, which must never copy from the source (the dynamic
	// types can differ); every subobject transfers individually.
	if (cls.is_polymorphic)
		return 0;
	// PA27: the shared virtual-base region is not part of the base
	// subobject's own storage; no whole-storage shortcut.
	if (ClassHasVBases(cls))
		return 0;
	// The prefix spans subobjects whose transfer of *this* form is
	// trivial: constructor forms check copy/move constructors, assign
	// forms check copy/move assignments (12.8p25/p28).
	unsigned long long end = 0;
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		if (!TransferTrivial(*row.cls, is_move, assign_form))
			return 0;
		if (!row.cls->is_empty)
		{
			if (row.offset + row.cls->size > end)
				end = row.offset + row.cls->size;
			if (row.cls->alignment > alignment)
				alignment = row.cls->alignment;
		}
	}
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty() && !field.is_bit_field)
			continue;
		if (field.name.empty() && field.bit_width == 0)
			continue;  // zero-width alignment row
		const ClassInfo* member = SubobjectClass(field.type);
		if (member && !TransferTrivial(*member, is_move, assign_form))
		{
			first_suffix = i;
			return field.offset;
		}
		// The synthesized assignment copies bit-field storage units
		// as scalar unit stores rather than raw storage bytes.
		if (assign_form && field.is_bit_field)
		{
			first_suffix = i;
			return field.offset;
		}
		unsigned long long size = field.is_bit_field
			? TypeSize(field.type) : TypeSize(field.type);
		if (field.offset + size > end)
			end = field.offset + size;
		if (!field.name.empty() && TypeAlignment(field.type) > alignment)
			alignment = TypeAlignment(field.type);
	}
	first_suffix = cls.fields.size();
	alignment = cls.alignment;
	return cls.size;
}

// The base subobjects transfer through their own special members, in
// declaration order (12.8p28 / 12.6.2p10).
void SemBinder::AppendBaseTransfer(const ClassInfo& cls, bool is_move,
                                   bool assign_form,
                                   const SemNode& source_proto,
                                   vector<SemNodePtr>& out)
{
	EValueCategory category = is_move ? VC_XVALUE : VC_LVALUE;
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		const ClassDirectBase& row = cls.direct_bases[b];
		// PA27: virtual rows belong to the complete-object phase; the
		// synthesized base entry transfers non-virtual subobjects only.
		if (row.is_virtual)
			continue;
		SemNodePtr base_source = MakeSemNode(SN_MEMBER_EXPRESSION);
		base_source->type = MakeNamedType(TK_CLASS, row.cls->entity);
		base_source->category = category;
		base_source->base_hops = 1;
		base_source->base_offset = row.offset;
		base_source->children.push_back(CloneSemNode(source_proto));
		if (assign_form)
		{
			SemValue lhs;
			lhs.node = MakeSemNode(SN_MEMBER_EXPRESSION);
			lhs.node->type = MakeNamedType(TK_CLASS, row.cls->entity);
			lhs.node->category = VC_LVALUE;
			lhs.node->base_hops = 1;
			lhs.node->base_offset = row.offset;
			lhs.node->children.push_back(ThisObjectExpr());
			lhs.type = lhs.node->type;
			lhs.category = VC_LVALUE;
			SemValue rhs;
			rhs.node = std::move(base_source);
			rhs.type = lhs.type;
			rhs.category = category;
			vector<SemValue> operands;
			operands.push_back(std::move(lhs));
			operands.push_back(std::move(rhs));
			SemValue result;
			if (!analyzer_.ResolveOperatorCall("=", operands, true,
			                                   result))
				throw runtime_error("no base assignment operator");
			SemNodePtr statement = MakeSemNode(SN_EXPRESSION_STATEMENT);
			statement->children.push_back(std::move(result.node));
			out.push_back(std::move(statement));
			continue;
		}
		SemValue source;
		source.node = std::move(base_source);
		source.type = MakeNamedType(TK_CLASS, row.cls->entity);
		source.category = category;
		vector<SemValue> args;
		args.push_back(std::move(source));
		int index = ResolveClassConstructor(*row.cls, args, false,
		                                    "base subobject");
		vector<SemNodePtr> arg_nodes;
		arg_nodes.push_back(std::move(args[0].node));
		out.push_back(MakeConstructorCall(*row.cls, index, true,
		                                  ThisBaseAddress(cls, b),
		                                  std::move(arg_nodes)));
	}
}

// Per-element transfer of a class array member through the element
// class's copy/move constructor.
void SemBinder::AppendMemberArrayTransfer(const ClassInfo& member,
                                          const ClassField& field,
                                          const SemNode& source_proto,
                                          EValueCategory category,
                                          vector<SemNodePtr>& out)
{
	TypePtr bare = RemoveTopCv(field.type);
	TypePtr element = RemoveTopCv(bare->target);
	for (unsigned long long j = 0; j < bare->bound; j++)
	{
		SemValue source;
		source.node = SubscriptNode(
			SourceFieldExpr(source_proto, field, category), j);
		source.node->category = category;
		source.type = element;
		source.category = category;
		vector<SemValue> args;
		args.push_back(std::move(source));
		int index = ResolveClassConstructor(member, args, false,
		                                    field.name.c_str());
		vector<SemNodePtr> arg_nodes;
		arg_nodes.push_back(std::move(args[0].node));
		out.push_back(MakeConstructorCall(
			member, index, false,
			AddressOfNode(SubscriptNode(ThisFieldExpr(field), j)),
			std::move(arg_nodes)));
	}
}

void SemBinder::AppendTransferActions(const ClassInfo& cls, bool is_move,
                                      bool assign_form,
                                      const SemNode& source_proto,
                                      vector<SemNodePtr>& out)
{
	EValueCategory category = is_move ? VC_XVALUE : VC_LVALUE;
	// PA27: a synthesized complete-object constructor default-
	// initializes the shared virtual bases first (base entries drop
	// the marked actions; assignment leaves the shared region alone).
	if (!assign_form)
		AppendVBaseInits(cls, 0, out, false);
	unsigned long long alignment = 1;
	size_t first_suffix = 0;
	unsigned long long span = TrivialStoragePrefix(
		cls, is_move, assign_form, alignment, first_suffix);
	// An empty object has no bytes to transfer (the PA15 convention).
	if (span && cls.is_empty)
		first_suffix = cls.fields.size();
	else if (span)
		out.push_back(StorageCopyAction(cls, source_proto, span,
		                                alignment));
	else if (cls.base)
	{
		AppendBaseTransfer(cls, is_move, assign_form, source_proto,
		                   out);
	}
	// PA17: a synthesized copy/move constructor stores this class's own
	// vpointer after the base transfer; assignment never changes the
	// established dynamic type.
	if (!assign_form && cls.is_polymorphic)
		out.push_back(MakeVPointerStore(cls));
	for (size_t i = first_suffix; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		const ClassInfo* member = SubobjectClass(field.type);
		TypePtr bare = RemoveTopCv(field.type);
		if (member && bare->kind == TK_ARRAY)
		{
			AppendMemberArrayTransfer(*member, field, source_proto,
			                          category, out);
			continue;
		}
		if (member && !assign_form)
		{
			SemValue source;
			source.node = SourceFieldExpr(source_proto, field, category);
			source.type = bare;
			source.category = category;
			vector<SemValue> args;
			args.push_back(std::move(source));
			int index = ResolveClassConstructor(*member, args, false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			arg_nodes.push_back(std::move(args[0].node));
			out.push_back(MakeConstructorCall(
				*member, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				std::move(arg_nodes)));
			continue;
		}
		if (member && assign_form)
		{
			SemValue lhs;
			lhs.node = ThisFieldExpr(field);
			lhs.type = bare;
			lhs.category = VC_LVALUE;
			SemValue rhs;
			rhs.node = SourceFieldExpr(source_proto, field, category);
			rhs.type = bare;
			rhs.category = category;
			vector<SemValue> operands;
			operands.push_back(std::move(lhs));
			operands.push_back(std::move(rhs));
			SemValue result;
			if (!analyzer_.ResolveOperatorCall("=", operands, true, result))
				throw runtime_error("no member assignment operator for " +
				                    field.name);
			SemNodePtr statement = MakeSemNode(SN_EXPRESSION_STATEMENT);
			statement->children.push_back(std::move(result.node));
			out.push_back(std::move(statement));
			continue;
		}
		if (assign_form && field.is_bit_field)
		{
			// One plain unit copy per bit-field storage unit; later
			// rows of the same unit are already covered.
			if (i > first_suffix && cls.fields[i - 1].is_bit_field &&
			    cls.fields[i - 1].offset == field.offset)
				continue;
			ClassField unit = field;
			unit.is_bit_field = false;
			unit.bit_offset = 0;
			unit.bit_width = 0;
			SemValue value;
			value.node = SourceFieldExpr(source_proto, unit, category);
			value.type = bare;
			value.category = VC_LVALUE;
			out.push_back(MemberAssignAction(
				unit, ThisFieldExpr(unit), std::move(value)));
			continue;
		}
		// Scalar, reference, and bit-field members transfer value-wise.
		SemValue value;
		value.node = SourceFieldExpr(source_proto, field,
		                             IsReferenceType(field.type)
		                             ? VC_LVALUE : category);
		value.type = IsReferenceType(field.type) ? field.type->target
		                                         : bare;
		value.category = VC_LVALUE;
		out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                 std::move(value)));
	}
}

void SemBinder::EnsureSpecialCtor(const ClassInfo& cls_in, int index,
                                  bool out_of_class)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	ClassCtor& ctor = cls.ctors[index];
	if (ctor.built)
		return;
	ctor.built = true;
	bool is_move = ctor.kind == CK_MOVE;
	const string& base_name = cls.members->name;
	Scope* fn_scope =
		model_.CreateScope(SCOPE_FUNCTION, base_name, cls.members);
	string param_name;
	if (!ctor.param_names.empty() && !ctor.param_names[0].empty())
		param_name = ctor.param_names[0];
	else
		// Implicit members name their source "other"; explicitly
		// defaulted declarations keep the positional spelling.
		param_name = ctor.implicit ? "other" : "__param1";
	DeferredBody body;
	body.name = base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = ctor.type;
	ParameterInfo parameter;
	parameter.name = param_name;
	parameter.type = ctor.type->parameters[0];
	body.composed.parameters.push_back(parameter);
	ScopeBinding param_binding;
	param_binding.kind = SB_PARAMETER;
	param_binding.name = param_name;
	param_binding.type = parameter.type;
	AddBinding(*fn_scope, param_binding);
	SemNodePtr item = BuildFunctionNode(body, SF_CONSTRUCTOR);
	item->synthesized = true;
	SemNode* node = item.get();

	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	current_ = fn_scope;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = base_name;
	method_.this_type = node->type->parameters[0];
	bf_units_written_.clear();
	vector<SemNodePtr> actions;
	try
	{
		SemNodePtr source = SourceObjectExpr(
			fn_scope, param_name,
			RemoveTopCv(parameter.type->target));
		AppendTransferActions(cls, is_move, false, *source, actions);
	}
	catch (...)
	{
		current_ = saved_scope;
		method_ = saved_method;
		throw;
	}
	current_ = saved_scope;
	method_ = saved_method;
	for (size_t i = 0; i < actions.size(); i++)
		node->children.push_back(std::move(actions[i]));
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	ctor.built_unwind_no = !may_throw;
	unit_.deferred.push_back(std::move(item));
}

void SemBinder::EnsureAssignSpecial(const NamedTypeInfo* cls_entity,
                                    size_t overload_index)
{
	ClassInfo* found = unit_.classes.Find(cls_entity);
	if (!found)
		return;
	BuildAssignSpecial(*found, overload_index, false);
}

void SemBinder::BuildAssignSpecial(ClassInfo& cls, size_t overload_index,
                                   bool out_of_class)
{
	bool is_move;
	if ((int)overload_index == cls.copy_assign_index)
		is_move = false;
	else if ((int)overload_index == cls.move_assign_index)
		is_move = true;
	else
		return;  // a user-declared overload defines itself
	bool& built = is_move ? cls.move_assign_built : cls.copy_assign_built;
	if (built)
		return;
	built = true;
	ScopeBinding* binding = FindOwnBinding(*cls.members, "operator =");
	const TypePtr& declared = overload_index == 0
		? binding->type : binding->overloads[overload_index - 1];
	const string name = "operator =";
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, name, cls.members);
	bool user_defaulted = overload_index < binding->fn_defaulted.size() &&
		binding->fn_defaulted[overload_index];
	DeferredBody body;
	body.name = name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = declared;
	ParameterInfo parameter;
	parameter.name = user_defaulted ? "__param1" : "other";
	parameter.type = declared->parameters[0];
	body.composed.parameters.push_back(parameter);
	ScopeBinding param_binding;
	param_binding.kind = SB_PARAMETER;
	param_binding.name = parameter.name;
	param_binding.type = parameter.type;
	AddBinding(*fn_scope, param_binding);
	SemNodePtr item = BuildFunctionNode(body, SF_NONE);
	item->synthesized = true;
	SemNode* node = item.get();

	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	current_ = fn_scope;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = name;
	method_.this_type = node->type->parameters[0];
	bf_units_written_.clear();
	vector<SemNodePtr> actions;
	try
	{
		SemNodePtr source = SourceObjectExpr(
			fn_scope, parameter.name,
			RemoveTopCv(parameter.type->target));
		AppendTransferActions(cls, is_move, true, *source, actions);
		SemNodePtr ret = MakeSemNode(SN_RETURN_STATEMENT);
		ret->children.push_back(ThisObjectExpr());
		actions.push_back(std::move(ret));
	}
	catch (...)
	{
		current_ = saved_scope;
		method_ = saved_method;
		throw;
	}
	current_ = saved_scope;
	method_ = saved_method;
	for (size_t i = 0; i < actions.size(); i++)
		node->children.push_back(std::move(actions[i]));
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	if (is_move)
		cls.move_assign_unwind_no = !may_throw;
	else
		cls.copy_assign_unwind_no = !may_throw;
	if (overload_index < binding->fn_unwind_no.size())
		binding->fn_unwind_no[overload_index] = !may_throw;
	// An implicitly declared assignment's computed fact is its
	// implicit exception specification (15.4p14).
	if (overload_index < binding->fn_noexcept_decl.size())
		binding->fn_noexcept_decl[overload_index] = !may_throw;
	if (out_of_class)
		// A source-owned defaulted definition prints unconditionally.
		AppendItem(std::move(item));
	else
		unit_.deferred.push_back(std::move(item));
}

void SemBinder::RecomputeUserCtorFact(ClassInfo& cls)
{
	cls.has_user_ctor = false;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		const ClassCtor& ctor = cls.ctors[i];
		if (!ctor.implicit && !ctor.defaulted && !ctor.deleted)
			cls.has_user_ctor = true;
	}
	// The caller flipped defaulted/defaulted_outside facts on a
	// completed class; drop every memoized fact derived from them.
	InvalidateClassFacts();
}

int SemBinder::ResolveClassCtorHost(const ClassInfo& cls,
                                    vector<SemValue>& args, bool copy_init,
                                    const char* what)
{
	return ResolveClassConstructor(cls, args, copy_init, what);
}
