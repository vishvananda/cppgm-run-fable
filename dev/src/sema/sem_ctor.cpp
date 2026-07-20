#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 constructor and destructor synthesis: mem-initializer
// analysis, default member initialization, destructor epilogues, and
// the implicitly-defined special members (12.1, 12.4, 12.6.2).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

}  // namespace

// PA25: the demand-synthesis seam for a selected copy/move
// constructor whose body a keeping context (new-expressions) needs.
void SemBinder::EnsureSpecialCtorHost(const ClassInfo& cls, int index)
{
	const ClassCtor& selected = cls.ctors[index];
	if ((selected.kind == CK_COPY || selected.kind == CK_MOVE) &&
	    !selected.definition &&
	    (selected.implicit || selected.defaulted))
		EnsureSpecialCtor(cls, index);
}

// --- constructor member initialization -------------------------------------

// 15.2p2 / 12.6.2p10: once a constructor finishes constructing this
// subobject, an exception from a later initializer (or the body) must
// destroy it before leaving the constructor. The pinned destructor
// action rides outside `children` (like result_dtor) so the dump and
// the may-throw walks see only the construction; the lowering arms it
// as an unwind-only cleanup after the construction runs.
void SemBinder::ArmSubobjectCleanup(SemNode& action,
                                    const ClassInfo& subobject,
                                    bool base_entry, SemNodePtr address)
{
	if (!unit_.classes.DestructionHasEffects(subobject))
		return;
	action.subobject_dtor =
		MakeDestructorCall(subobject, base_entry, std::move(address));
}

// 12.6.2 class-type member: the aggregate braced form, 8.5p10
// value-initialization, or direct/list-initialization by constructor.
void SemBinder::AppendClassMemberInit(const ClassField& field,
                                      const ClassInfo& member_cls,
                                      const AstExpr* braced,
                                      const vector<const AstExpr*>& args,
                                      vector<SemNodePtr>& out)
{
	TypePtr bare = RemoveTopCv(field.type);
	if (braced && member_cls.is_aggregate)
	{
		SemNodePtr proto = ThisFieldExpr(field);
		AppendAggregateInit(member_cls, *proto, *braced, out);
		return;
	}
	if (!braced && args.empty())
	{
		// 8.5p10 `member()`: zero-initialization, then the default
		// constructor when one is needed.
		SemNodePtr zero_stmt = MakeSemNode(SN_EXPRESSION_STATEMENT);
		SemNodePtr assign = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
		assign->type = bare;
		assign->category = VC_LVALUE;
		assign->has_op = true;
		assign->op = OP_ASS;
		assign->op_spelling = "=";
		if (member_cls.has_user_ctor ||
		    unit_.classes.NeedsConstruction(member_cls))
		{
			// The zero-fill folds into the constructor action so both
			// share one member address.
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			SemNodePtr action = MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				std::move(arg_nodes));
			// 8.5p7: zero-initialization precedes the constructor
			// only when the selected default constructor is not
			// user-provided.
			bool user_provided = index >= 0 &&
				!member_cls.ctors[index].implicit &&
				!member_cls.ctors[index].defaulted;
			if (!user_provided)
			{
				action->has_value = true;
				action->value = ConstValue(FT_UNSIGNED_LONG_INT, member_cls.size);
			}
			ArmSubobjectCleanup(*action, member_cls, false,
			                    AddressOfNode(ThisFieldExpr(field)));
			out.push_back(std::move(action));
			return;
		}
		SemValue zero = ZeroValue(MakeFundamentalType(FT_INT));
		assign->children.push_back(ThisFieldExpr(field));
		assign->children.push_back(std::move(zero.node));
		zero_stmt->children.push_back(std::move(assign));
		out.push_back(std::move(zero_stmt));
		return;
	}
	{
		// Direct- or list-initialization by constructor.
		vector<SemValue> values;
		if (braced)
		{
			vector<const AstExpr*> items;
			for (size_t i = 0; i < braced->arguments.size(); i++)
				items.push_back(braced->arguments[i].get());
			AnalyzeInitArguments(items, values);
		}
		else
			AnalyzeInitArguments(args, values);
		if (values.size() == 1 && values[0].node &&
		    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
		    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
		    RemoveTopCv(values[0].type)->named == member_cls.entity)
		{
			SemNodePtr action = std::move(values[0].node);
			SemNode& call = *action->children[0];
			call.children.insert(
				call.children.begin() + 1,
				AddressOfNode(ThisFieldExpr(field)));
			action->ctor_addressed = true;
			ArmSubobjectCleanup(*action, member_cls, false,
			                    AddressOfNode(ThisFieldExpr(field)));
			out.push_back(std::move(action));
			return;
		}
		if (values.size() == 1 && values[0].node &&
		    values[0].category == VC_PRVALUE &&
		    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
		    RemoveTopCv(values[0].type)->named == member_cls.entity)
		{
			// A same-class prvalue (call result) constructs the member
			// directly (copy elision).
			SemNodePtr assign = MakeSemNode(SN_ASSIGNMENT_EXPRESSION);
			assign->type = bare;
			assign->category = VC_LVALUE;
			assign->has_op = true;
			assign->op = OP_ASS;
			assign->op_spelling = "=";
			assign->children.push_back(ThisFieldExpr(field));
			assign->children.push_back(std::move(values[0].node));
			SemNodePtr statement = MakeSemNode(SN_EXPRESSION_STATEMENT);
			statement->children.push_back(std::move(assign));
			out.push_back(std::move(statement));
			return;
		}
		int index = ResolveClassConstructor(member_cls, values, false,
		                                    field.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < values.size(); i++)
			arg_nodes.push_back(std::move(values[i].node));
		SemNodePtr action = MakeConstructorCall(
			member_cls, index, false,
			AddressOfNode(ThisFieldExpr(field)), std::move(arg_nodes));
		ArmSubobjectCleanup(*action, member_cls, false,
		                    AddressOfNode(ThisFieldExpr(field)));
		out.push_back(std::move(action));
		return;
	}
}

void SemBinder::AppendMemberInit(const ClassInfo& cls,
                                 const ClassField& field,
                                 const AstInitializer* init,
                                 vector<SemNodePtr>& out)
{
	(void)cls;
	// Collect the initializer's argument expressions.
	vector<const AstExpr*> args;
	const AstExpr* braced = 0;
	if (init->kind == INIT_PAREN)
	{
		for (size_t i = 0; i < init->args.size(); i++)
			args.push_back(init->args[i].get());
	}
	else if (init->kind == INIT_EQ)
	{
		if (init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		else
			args.push_back(init->expr.get());
	}
	else if (init->kind == INIT_BRACED)
		braced = init->expr.get();
	else
		throw OutsideBoundary("member initializer form");

	TypePtr bare = RemoveTopCv(field.type);
	const ClassInfo* member_cls = bare->kind == TK_CLASS
		? unit_.classes.Find(bare->named) : 0;
	if (member_cls)
	{
		AppendClassMemberInit(field, *member_cls, braced, args, out);
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
		if (!braced && args.empty())
		{
			// 8.5p10: `arr()` value-initializes every element.
			TypePtr element = RemoveTopCv(bare->target);
			if (element->kind == TK_CLASS)
				throw OutsideBoundary("class array member initializer");
			for (unsigned long long i = 0; i < bare->bound; i++)
			{
				ClassField element_field;
				element_field.name = field.name;
				element_field.type = element;
				out.push_back(MemberAssignAction(
					element_field,
					SubscriptNode(ThisFieldExpr(field), i),
					ZeroValue(element)));
			}
			return;
		}
		if (!braced)
			throw OutsideBoundary("array member initializer form");
		AppendArrayMemberInit(field, braced, out);
		return;
	}
	// Scalar / reference / pointer member.
	if (braced)
	{
		if (braced->arguments.size() > 1)
			throw runtime_error("too many initializers for " + field.name);
		if (braced->arguments.empty())
		{
			out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
			                                 ZeroValue(bare)));
			return;
		}
		SemValue value = analyzer_.Analyze(*braced->arguments[0]);
		CheckListInitNarrowing(value, bare);
		out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                 std::move(value)));
		return;
	}
	// PA34: argument-level `pattern...` items expand in place before
	// the arity checks (`t(u...)` over a scalar member).
	vector<SemValue> values;
	for (size_t i = 0; i < args.size(); i++)
	{
		if (args[i]->kind == EK_PACK_EXPANSION)
		{
			if (!ExpandPackExpression(*args[i]->operands[0], values))
				throw runtime_error("member initializer pack "
				                    "expansion names no expandable "
				                    "pack");
			continue;
		}
		values.push_back(analyzer_.Analyze(*args[i]));
	}
	if (values.empty())
	{
		// 8.5p10 `x()` value-initialization of a scalar member.
		if (IsReferenceType(field.type))
			throw runtime_error("reference member requires a binding");
		out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
		                                 ZeroValue(bare)));
		return;
	}
	if (values.size() != 1)
		throw runtime_error("too many initializers for " + field.name);
	out.push_back(MemberAssignAction(field, ThisFieldExpr(field),
	                                 std::move(values[0])));
}

// A zero-valued prvalue of the scalar type (value-initialization).
SemValue SemBinder::ZeroValue(const TypePtr& type)
{
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = "0";
	value.category = VC_PRVALUE;
	if (type->kind == TK_POINTER || IsNullPtrType(type))
	{
		value.type = type;
		value.node->type = type;
		value.node->null_pointer = true;
		return value;
	}
	EFundamentalType fundamental = type->kind == TK_ENUM
		? type->named->enum_underlying
		: type->fundamental;
	value.type = type;
	value.node->type = type;
	value.node->has_value = true;
	value.node->value = ConstValue(fundamental, 0);
	value.null_pointer_literal = IsIntegralType(type);
	return value;
}

// 8.5.4p7 narrowing rejection for the list-initialization forms the
// tests pin: floating -> integral, integral -> floating (non-constant),
// and width-losing integral conversions of non-constant values.
void SemBinder::CheckListInitNarrowing(const SemValue& value,
                                       const TypePtr& dest)
{
	if (!IsArithmeticType(dest) || !IsArithmeticType(value.type))
		return;
	bool from_float = IsFloatingFundamental(value.type->fundamental);
	bool to_float = IsFloatingFundamental(dest->fundamental);
	if (from_float && !to_float)
		throw runtime_error("narrowing conversion in list-initialization");
	bool constant = value.node && value.node->has_value;
	if (constant)
	{
		// A constant that converts back unchanged does not narrow.
		ConstValue converted =
			ConvertConstValue(value.node->value, dest->fundamental);
		ConstValue back =
			ConvertConstValue(converted, value.node->value.type);
		if (back.bits != value.node->value.bits)
			throw runtime_error("narrowing conversion in "
			                    "list-initialization");
		return;
	}
	if (!from_float && to_float)
		throw runtime_error("narrowing conversion in list-initialization");
	if (!from_float && !to_float &&
	    (TypeSize(dest) < TypeSize(value.type) ||
	     (TypeSize(dest) == TypeSize(value.type) &&
	      IsSignedIntegralFundamental(dest->fundamental) !=
	      IsSignedIntegralFundamental(value.type->fundamental))))
		throw runtime_error("narrowing conversion in list-initialization");
}

void SemBinder::AppendArrayMemberInit(const ClassField& field,
                                      const AstExpr* braced,
                                      vector<SemNodePtr>& out)
{
	TypePtr array = RemoveTopCv(field.type);
	TypePtr element = RemoveTopCv(array->target);
	if (element->kind == TK_CLASS)
		throw OutsideBoundary("class array member initializer");
	if (braced->arguments.size() > array->bound)
		throw runtime_error("too many initializers for " + field.name);
	for (unsigned long long i = 0; i < array->bound; i++)
	{
		ClassField element_field;
		element_field.name = field.name;
		element_field.type = element;
		SemNodePtr target = SubscriptNode(ThisFieldExpr(field), i);
		if (i < braced->arguments.size())
		{
			SemValue value = analyzer_.Analyze(*braced->arguments[i]);
			CheckListInitNarrowing(value, element);
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 std::move(value)));
		}
		else
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 ZeroValue(element)));
	}
}

void SemBinder::AppendFieldDefaultInit(const ClassInfo& cls,
                                       const ClassField& field,
                                       vector<SemNodePtr>& out)
{
	if (field.default_init)
	{
		AppendMemberInit(cls, field, field.default_init, out);
		return;
	}
	// 9.5/12.6.2p8: variant members are not default-initialized.
	if (field.from_union)
		return;
	TypePtr bare = RemoveTopCv(field.type);
	if (bare->kind == TK_CLASS)
	{
		const ClassInfo* member_cls = unit_.classes.Find(bare->named);
		if (!member_cls)
			return;
		// 12.6.2p8: a union member without a user-declared default
		// constructor is not default-initialized (the storage stays
		// uninitialized; a variant member's nontrivial constructor
		// never runs implicitly).
		if (member_cls->is_union)
		{
			bool declared_default = false;
			for (size_t i = 0; i < member_cls->ctors.size(); i++)
				if (member_cls->ctors[i].kind == CK_ORDINARY &&
				    !member_cls->ctors[i].implicit &&
				    member_cls->ctors[i].type->parameters.empty())
					declared_default = true;
			if (!declared_default)
				return;
		}
		if (member_cls->has_user_ctor ||
		    unit_.classes.NeedsConstruction(*member_cls))
		{
			if (!unit_.classes.DefaultConstructionHasEffects(
					*member_cls))
			{
				AppendElidedCtorDemand(*member_cls, false, out);
				// The subobject still constructs (worklessly): a later
				// initializer's throw must destroy it (15.2p2).
				if (unit_.classes.DestructionHasEffects(*member_cls))
				{
					SemNodePtr marker =
						MakeSemNode(SN_CONSTRUCTOR_ACTION);
					marker->trivial_init = true;
					ArmSubobjectCleanup(
						*marker, *member_cls, false,
						AddressOfNode(ThisFieldExpr(field)));
					out.push_back(std::move(marker));
				}
				return;
			}
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			SemNodePtr action = MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(ThisFieldExpr(field)),
				std::move(arg_nodes));
			ArmSubobjectCleanup(*action, *member_cls, false,
			                    AddressOfNode(ThisFieldExpr(field)));
			out.push_back(std::move(action));
		}
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
		TypePtr element = RemoveTopCv(bare->target);
		while (element->kind == TK_ARRAY)
			element = RemoveTopCv(element->target);
		if (element->kind != TK_CLASS)
			return;
		const ClassInfo* member_cls = unit_.classes.Find(element->named);
		if (!member_cls || (!member_cls->has_user_ctor &&
		                    !unit_.classes.NeedsConstruction(*member_cls)))
			return;
		if (!unit_.classes.DefaultConstructionHasEffects(*member_cls))
		{
			AppendElidedCtorDemand(*member_cls, false, out);
			return;
		}
		if (bare->target->kind == TK_ARRAY)
			throw OutsideBoundary("multidimensional class member array");
		for (unsigned long long i = 0; i < bare->bound; i++)
		{
			vector<SemValue> no_args;
			int index = ResolveClassConstructor(*member_cls, no_args,
			                                    false,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < no_args.size(); j++)
				arg_nodes.push_back(std::move(no_args[j].node));
			SemNodePtr action = MakeConstructorCall(
				*member_cls, index, false,
				AddressOfNode(SubscriptNode(ThisFieldExpr(field), i)),
				std::move(arg_nodes));
			ArmSubobjectCleanup(
				*action, *member_cls, false,
				AddressOfNode(SubscriptNode(ThisFieldExpr(field), i)));
			out.push_back(std::move(action));
		}
		return;
	}
	if (IsReferenceType(field.type))
		throw runtime_error("reference member is not initialized");
}

// A full-expression temporary's destructor action: no address subtree
// (the lowering destroys the materialized slot it created).
// 12.6.2p8 implicit default-initialization of the base subobject. A
// chain that performs no observable work is elided from the synthesized
// body (the reference emission); the user-provided constructors it
// would have reached are still resolved and demanded. A user-written
// constructor body elides through the syntactic walk (instantiated
// pattern bodies count); synthesized bodies keep the conservative
// point-of-instantiation fact (the pinned member-call shape).
void SemBinder::AppendBaseDefaultInit(const ClassInfo& cls,
                                      vector<SemNodePtr>& out,
                                      bool syntactic)
{
	// PA26: every direct base default-initializes in declaration order.
	// PA27: virtual rows belong to the complete-object phase
	// (AppendVBaseInits).
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
		if (!cls.direct_bases[b].is_virtual)
			AppendOneBaseDefaultInit(cls, b, out, syntactic);
}

void SemBinder::AppendOneBaseDefaultInit(const ClassInfo& cls,
                                         size_t base_index,
                                         vector<SemNodePtr>& out,
                                         bool syntactic)
{
	const ClassInfo& base = *cls.direct_bases[base_index].cls;
	if (!base.has_user_ctor && !unit_.classes.NeedsConstruction(base))
		return;
	if (syntactic
	        ? !unit_.classes.DefaultConstructionHasSyntacticEffects(base)
	        : !unit_.classes.DefaultConstructionHasEffects(base))
	{
		AppendElidedCtorDemand(base, true, out);
		return;
	}
	vector<SemValue> no_args;
	int index = ResolveClassConstructor(base, no_args, false,
	                                    "base subobject");
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < no_args.size(); j++)
		arg_nodes.push_back(std::move(no_args[j].node));
	SemNodePtr action = MakeConstructorCall(
		base, index, true, ThisBaseAddress(cls, base_index),
		std::move(arg_nodes));
	ArmSubobjectCleanup(*action, base, true,
	                    ThisBaseAddress(cls, base_index));
	out.push_back(std::move(action));
}

// The demand side of an elided subobject chain: the nearest
// user-provided default constructors are resolved (each in its own
// implicit constructor's access context) and recorded as elided
// actions, so the lowering still emits the demanded definitions.
void SemBinder::AppendElidedCtorDemand(const ClassInfo& cls,
                                       bool base_entry,
                                       vector<SemNodePtr>& out)
{
	if (cls.has_user_ctor)
	{
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    "base subobject");
		SemNodePtr action = MakeConstructorCall(cls, index, base_entry,
		                                        SemNodePtr(),
		                                        vector<SemNodePtr>());
		action->elided = true;
		out.push_back(std::move(action));
		return;
	}
	MethodContext saved = method_;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = saved.fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = cls.members->name;
	try
	{
		for (size_t b = 0; b < cls.direct_bases.size(); b++)
			AppendElidedCtorDemand(*cls.direct_bases[b].cls, true, out);
		for (size_t i = 0; i < cls.fields.size(); i++)
		{
			const ClassInfo* member =
				unit_.classes.MemberClass(cls.fields[i].type);
			if (member && (member->has_user_ctor ||
			               unit_.classes.NeedsConstruction(*member)))
				AppendElidedCtorDemand(*member, false, out);
		}
	}
	catch (...)
	{
		method_ = saved;
		throw;
	}
	method_ = saved;
}

SemNodePtr SemBinder::MakeTemporaryDtor(const ClassInfo& cls)
{
	return MakeDestructorCall(cls, false, SemNodePtr());
}

// PA26: the collected written mem-initializers of one constructor.
struct SemBinder::MemberInitPlan
{
	std::map<string, const AstMemInitializer*> by_field;
	vector<const AstMemInitializer*> base_inits;  // by base index
	vector<Scope*> base_init_scopes;  // pack-expansion elements
	size_t base_init_count = 0;
};

// PA27: the shared virtual-base subobject address at a table index.
SemNodePtr SemBinder::ThisVBaseAddress(const ClassInfo& cls,
                                       size_t vbase_index)
{
	const ClassVBase& row = cls.vbases[vbase_index];
	SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
	member->type = MakeNamedType(TK_CLASS, row.cls->entity);
	member->category = VC_LVALUE;
	member->base_hops = 1;
	member->base_offset = row.offset;
	member->children.push_back(ThisObjectExpr());
	return AddressOfNode(std::move(member));
}

// PA27 12.6.2p10: the shared virtual bases initialize first, in vbase
// table order, only in the complete-object constructor. Direct virtual
// rows may carry a written mem-initializer; every other entry
// default-initializes with the usual whole-chain elision.
void SemBinder::AppendVBaseInits(const ClassInfo& cls,
                                 const MemberInitPlan* plan,
                                 vector<SemNodePtr>& out, bool syntactic)
{
	for (size_t v = 0; v < cls.vbases.size(); v++)
	{
		const ClassInfo& base = *cls.vbases[v].cls;
		const AstMemInitializer* init = 0;
		if (plan)
			for (size_t b = 0; b < cls.direct_bases.size(); b++)
				if (cls.direct_bases[b].is_virtual &&
				    cls.direct_bases[b].cls == &base)
				{
					init = plan->base_inits[b];
					break;
				}
		size_t mark = out.size();
		if (init)
		{
			vector<SemValue> values;
			const AstInitializer& args = *init->init;
			if (args.kind == INIT_PAREN)
				analyzer_.AnalyzeArgumentList(args.args, values);
			else if (args.kind == INIT_BRACED)
				analyzer_.AnalyzeArgumentList(args.expr->arguments,
				                              values);
			int index = ResolveClassConstructor(base, values, false,
			                                    "base initializer");
			vector<SemNodePtr> arg_nodes;
			for (size_t i = 0; i < values.size(); i++)
				arg_nodes.push_back(std::move(values[i].node));
			SemNodePtr action = MakeConstructorCall(
				base, index, true, ThisVBaseAddress(cls, v),
				std::move(arg_nodes));
			ArmSubobjectCleanup(*action, base, true,
			                    ThisVBaseAddress(cls, v));
			out.push_back(std::move(action));
		}
		else
		{
			if (!base.has_user_ctor &&
			    !unit_.classes.NeedsConstruction(base))
				continue;
			if (syntactic
			        ? !unit_.classes
			               .DefaultConstructionHasSyntacticEffects(base)
			        : !unit_.classes.DefaultConstructionHasEffects(base))
			{
				AppendElidedCtorDemand(base, true, out);
			}
			else
			{
				vector<SemValue> no_args;
				int index = ResolveClassConstructor(base, no_args, false,
				                                    "base subobject");
				vector<SemNodePtr> arg_nodes;
				for (size_t j = 0; j < no_args.size(); j++)
					arg_nodes.push_back(std::move(no_args[j].node));
				SemNodePtr action = MakeConstructorCall(
					base, index, true, ThisVBaseAddress(cls, v),
					std::move(arg_nodes));
				ArmSubobjectCleanup(*action, base, true,
				                    ThisVBaseAddress(cls, v));
				out.push_back(std::move(action));
			}
		}
		for (size_t i = mark; i < out.size(); i++)
			out[i]->vbase_action = true;
	}
}

// Collects the written mem-initializers: field initializers by name,
// base initializers by direct-base index (pack-expanded entries carry
// their element scopes). A delegating constructor analyzes in place;
// the caller then returns without further actions (12.6.2p6).
bool SemBinder::CollectMemberInits(const DeferredBody& body, SemNode& item,
                                   MemberInitPlan& plan)
{
	const ClassInfo& cls = *body.cls;
	for (size_t i = 0; i < body.decl->mem_initializers.size(); i++)
	{
		const AstMemInitializer& mem = body.decl->mem_initializers[i];
		if (mem.pack)
		{
			// 12.6.2p15: one initializer per pack-expanded base element.
			std::vector<std::pair<TypePtr, Scope*>> elements;
			ExpandPackMemInitTargets(mem, elements);
			for (size_t k = 0; k < elements.size(); k++)
			{
				TypePtr element = elements[k].first;
				size_t base_index = cls.direct_bases.size();
				if (element->kind == TK_CLASS)
					for (size_t b = 0; b < cls.direct_bases.size(); b++)
						if (element->named ==
						    cls.direct_bases[b].cls->entity)
						{
							base_index = b;
							break;
						}
				if (base_index == cls.direct_bases.size())
					throw runtime_error(
						"member initializer names no member or base");
				if (plan.base_inits[base_index])
					throw runtime_error("duplicate base initializer");
				plan.base_inits[base_index] = &mem;
				plan.base_init_scopes[base_index] = elements[k].second;
			}
			plan.base_init_count++;
			continue;
		}
		if (mem.id.IsPlainIdentifier() &&
		    FindClassField(cls, mem.id.parts[0].identifier))
		{
			const string& name = mem.id.parts[0].identifier;
			if (plan.by_field.count(name))
				throw runtime_error("duplicate member initializer " + name);
			plan.by_field[name] = &mem;
			continue;
		}
		TypePtr named = ResolveTypeName(mem.id);
		if (named->kind == TK_CLASS && named->named == cls.entity)
		{
			// 12.6.2p6: a mem-initializer naming the class itself
			// delegates; no other initializer may appear.
			if (body.decl->mem_initializers.size() != 1)
				throw runtime_error("delegating constructor has other "
				                    "mem-initializers");
			vector<SemValue> values;
			const AstInitializer& init = *mem.init;
			vector<const AstExpr*> items;
			if (init.kind == INIT_PAREN)
				for (size_t j = 0; j < init.args.size(); j++)
					items.push_back(init.args[j].get());
			else if (init.kind == INIT_BRACED)
				for (size_t j = 0;
				     j < init.expr->arguments.size(); j++)
					items.push_back(init.expr->arguments[j].get());
			AnalyzeInitArguments(items, values);
			int index = ResolveClassConstructor(
				cls, values, false, "delegating constructor");
			DowngradePackDeducedTemporaries(cls.ctors[index], values);
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < values.size(); j++)
				arg_nodes.push_back(std::move(values[j].node));
			item.children.push_back(MakeConstructorCall(
				cls, index, false,
				AddressOfNode(ThisObjectExpr()),
				std::move(arg_nodes)));
			return true;
		}
		size_t base_index = cls.direct_bases.size();
		if (named->kind == TK_CLASS)
			for (size_t b = 0; b < cls.direct_bases.size(); b++)
				if (named->named == cls.direct_bases[b].cls->entity)
				{
					base_index = b;
					break;
				}
		if (base_index < cls.direct_bases.size())
		{
			if (plan.base_inits[base_index])
				throw runtime_error("duplicate base initializer");
			plan.base_inits[base_index] = &mem;
			plan.base_init_count++;
			continue;
		}
		throw runtime_error("member initializer names no member or base");
	}
	return false;
}

void SemBinder::AnalyzeMemberInits(const DeferredBody& body, SemNode& item)
{
	const ClassInfo& cls = *body.cls;
	MemberInitPlan plan;
	plan.base_inits.assign(cls.direct_bases.size(),
	                       (const AstMemInitializer*)0);
	plan.base_init_scopes.assign(cls.direct_bases.size(), (Scope*)0);
	if (CollectMemberInits(body, item, plan))
		return;
	bf_units_written_.clear();
	vector<SemNodePtr> actions;
	// PA27 12.6.2p10: the shared virtual bases initialize ahead of the
	// direct non-virtual bases (complete-object constructors only).
	AppendVBaseInits(cls, &plan, actions, true);
	// 12.6.2p10: direct bases initialize in declaration order, each
	// through its mem-initializer or by default-initialization.
	for (size_t b = 0; b < cls.direct_bases.size(); b++)
	{
		if (cls.direct_bases[b].is_virtual)
			continue;
		if (!plan.base_inits[b])
		{
			AppendOneBaseDefaultInit(cls, b, actions, true);
			continue;
		}
		const ClassInfo& base = *cls.direct_bases[b].cls;
		vector<SemValue> values;
		const AstInitializer& init = *plan.base_inits[b]->init;
		// A pack-expanded initializer's arguments analyze under the
		// element scope (the mentioned packs re-bound to the element).
		Scope* saved_scope = current_;
		// 12.6.2p7: a braced initializer of an aggregate base
		// list-initializes the subobject (8.5.1), like the member form.
		if (init.kind == INIT_BRACED && base.is_aggregate &&
		    !base.has_user_ctor)
		{
			SemNodePtr proto = MakeSemNode(SN_MEMBER_EXPRESSION);
			proto->type = MakeNamedType(TK_CLASS, base.entity);
			proto->category = VC_LVALUE;
			proto->base_hops = 1;
			proto->base_offset = cls.direct_bases[b].offset;
			proto->children.push_back(ThisObjectExpr());
			if (plan.base_init_scopes[b])
				current_ = plan.base_init_scopes[b];
			try
			{
				AppendAggregateInit(base, *proto, *init.expr, actions);
			}
			catch (...)
			{
				current_ = saved_scope;
				throw;
			}
			current_ = saved_scope;
			continue;
		}
		if (plan.base_init_scopes[b])
			current_ = plan.base_init_scopes[b];
		try
		{
			// The list analyzer expands argument-level `pattern...`
			// items in place (14.5.3).
			if (init.kind == INIT_PAREN)
				analyzer_.AnalyzeArgumentList(init.args, values);
			else if (init.kind == INIT_BRACED)
				analyzer_.AnalyzeArgumentList(init.expr->arguments,
				                              values);
		}
		catch (...)
		{
			current_ = saved_scope;
			throw;
		}
		current_ = saved_scope;
		int index = ResolveClassConstructor(base, values, false,
		                                    "base initializer");
		vector<SemNodePtr> arg_nodes;
		for (size_t i = 0; i < values.size(); i++)
			arg_nodes.push_back(std::move(values[i].node));
		SemNodePtr action = MakeConstructorCall(base, index, true,
		                                        ThisBaseAddress(cls, b),
		                                        std::move(arg_nodes));
		ArmSubobjectCleanup(*action, base, true, ThisBaseAddress(cls, b));
		actions.push_back(std::move(action));
	}
	// PA17: the vpointer stores after base construction, before any
	// member initialization.
	if (cls.is_polymorphic)
		actions.push_back(MakeVPointerStore(cls));
	size_t matched = plan.base_init_count;
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		std::map<string, const AstMemInitializer*>::const_iterator found =
			plan.by_field.find(field.name);
		if (found != plan.by_field.end())
		{
			matched++;
			AppendMemberInit(cls, field, found->second->init.get(),
			                 actions);
		}
		else if (!cls.is_union && !field.anonymous_storage)
			// 12.6.2p8: union variant members are not implicitly
			// initialized; an anonymous aggregate's storage row
			// initializes through its injected member rows.
			AppendFieldDefaultInit(cls, field, actions);
	}
	if (matched != body.decl->mem_initializers.size())
		throw runtime_error("member initializer names no member");
	for (size_t i = 0; i < actions.size(); i++)
		item.children.push_back(std::move(actions[i]));
}

void SemBinder::AnalyzeDtorEpilogue(const ClassInfo& cls, SemNode& item)
{
	vector<SemNodePtr> actions;
	for (size_t i = cls.fields.size(); i-- > 0;)
	{
		const ClassField& field = cls.fields[i];
		// 9.5: variant members are not destroyed implicitly.
		if (field.from_union)
			continue;
		TypePtr bare = RemoveTopCv(field.type);
		if (bare->kind == TK_ARRAY)
		{
			TypePtr element = RemoveTopCv(bare->target);
			const ClassInfo* member_cls = element->kind == TK_CLASS
				? unit_.classes.Find(element->named) : 0;
			if (!member_cls ||
			    !unit_.classes.DestructionHasEffects(*member_cls))
				continue;
			for (unsigned long long j = bare->bound; j-- > 0;)
				actions.push_back(MakeDestructorCall(
					*member_cls, false,
					AddressOfNode(SubscriptNode(ThisFieldExpr(field),
					                            j))));
			continue;
		}
		const ClassInfo* member_cls = bare->kind == TK_CLASS
			? unit_.classes.Find(bare->named) : 0;
		if (!member_cls ||
		    !unit_.classes.DestructionHasEffects(*member_cls))
			continue;
		actions.push_back(MakeDestructorCall(
			*member_cls, false, AddressOfNode(ThisFieldExpr(field))));
	}
	// 12.4p7: direct bases destroy in reverse declaration order.
	for (size_t b = cls.direct_bases.size(); b-- > 0;)
	{
		if (cls.direct_bases[b].is_virtual)
			continue;
		const ClassInfo& base = *cls.direct_bases[b].cls;
		if (unit_.classes.DestructionHasEffects(base))
			actions.push_back(MakeDestructorCall(
				base, true, ThisBaseAddress(cls, b)));
	}
	// PA27 12.4p7: the complete-object destructor destroys the shared
	// virtual bases last, in reverse table order.
	for (size_t v = cls.vbases.size(); v-- > 0;)
	{
		const ClassInfo& base = *cls.vbases[v].cls;
		if (!unit_.classes.DestructionHasEffects(base))
			continue;
		SemNodePtr action =
			MakeDestructorCall(base, true, ThisVBaseAddress(cls, v));
		action->vbase_action = true;
		actions.push_back(std::move(action));
	}
	for (size_t i = 0; i < actions.size(); i++)
		item.children.push_back(std::move(actions[i]));
}

// The reference's delegating-argument presentation: an empty
// no-user-constructor temporary spelled as a functional cast drops
// its constructor call (and the odr-use) when the selected
// constructor template's parameter for that position deduced through
// a template-id holding a value-pack expansion
// (`index_sequence<I1...>`); concrete parameters keep the explicit
// call.
void SemBinder::DowngradePackDeducedTemporaries(const ClassCtor& ctor,
                                                vector<SemValue>& values)
{
	if (!ctor.tmpl_spec || !ctor.tmpl_spec->owner)
		return;
	const TemplateInfo& tmpl = *ctor.tmpl_spec->owner;
	for (size_t i = 0; i < values.size(); i++)
	{
		if (i >= tmpl.param_patterns.size())
			break;
		// A function-parameter pack breaks the positional alignment.
		if (i < tmpl.param_pattern_packs.size() &&
		    tmpl.param_pattern_packs[i])
			break;
		const TypePtr& pattern = tmpl.param_patterns[i];
		if (!pattern || pattern->kind != TK_TEMPLATE_SPEC)
			continue;
		bool value_pack = false;
		for (size_t a = 0; a < pattern->targs.size(); a++)
			if (pattern->targs[a].pack_pattern &&
			    pattern->targs[a].is_value)
				value_pack = true;
		if (!value_pack)
			continue;
		SemNode* node = values[i].node.get();
		if (!node || node->kind != SN_CONSTRUCTOR_ACTION ||
		    node->ctor_addressed || node->children.empty() ||
		    node->children[0]->children.size() != 1)
			continue;
		TypePtr bare = RemoveTopCv(values[i].type);
		if (bare->kind != TK_CLASS)
			continue;
		const ClassInfo* temp_cls = unit_.classes.Find(bare->named);
		if (!temp_cls || temp_cls->has_user_ctor ||
		    unit_.classes.NeedsConstruction(*temp_cls))
			continue;
		node->trivial_init = true;
	}
}

// --- implicit special members ----------------------------------------------

bool SemBinder::NodeMayThrow(const SemNode& node) const
{
	if (node.kind == SN_THROW)
		return true;
	if (node.kind == SN_CALL_EXPRESSION && !node.children.empty() &&
	    node.children[0]->kind == SN_CALLEE &&
	    !node.children[0]->unwind_no)
		return true;
	for (size_t i = 0; i < node.children.size(); i++)
		if (NodeMayThrow(*node.children[i]))
			return true;
	return false;
}

void SemBinder::EnsureImplicitDefaultCtor(const ClassInfo& cls_in,
                                          bool out_of_class)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	if (cls.implicit_ctor_built)
		return;
	// 12.1p6: an explicitly-defaulted default constructor is served by
	// the implicit body even when other user constructors exist.
	bool defaulted_default = false;
	for (size_t i = 0; i < cls.ctors.size(); i++)
		if (cls.ctors[i].defaulted && !cls.ctors[i].deleted &&
		    cls.ctors[i].type->parameters.empty())
			defaulted_default = true;
	if (cls.has_user_ctor && !defaulted_default)
		throw runtime_error("no default constructor for " +
		                    cls.entity->display);
	cls.implicit_ctor_built = true;
	const string& base_name = cls.members->name;
	Scope* fn_scope =
		model_.CreateScope(SCOPE_FUNCTION, base_name, cls.members);
	TypePtr ctor_type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                     vector<TypePtr>(), false);
	DeferredBody body;
	body.name = base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = ctor_type;
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
		AppendVBaseInits(cls, 0, actions, false);
		if (cls.base)
			AppendBaseDefaultInit(cls, actions, false);
		if (cls.is_polymorphic)
			actions.push_back(MakeVPointerStore(cls));
		for (size_t i = 0; i < cls.fields.size(); i++)
			if (!cls.is_union &&
			    (!cls.fields[i].name.empty() ||
			     !cls.fields[i].is_bit_field))
				AppendFieldDefaultInit(cls, cls.fields[i], actions);
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
	bool spec_may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
	{
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
		// 15.4p14: the computed specification reads the subobject
		// constructors' declared specifications (the 5.3.7 noexcept
		// walk), not their body facts.
		if (SemTreeMayThrow(*node->children[i]))
			spec_may_throw = true;
	}
	node->unwind_no = !may_throw;
	cls.implicit_ctor_unwind_no = !may_throw;
	cls.implicit_ctor_noexcept = !spec_may_throw;
	unit_.deferred.push_back(std::move(item));
}

void SemBinder::EnsureImplicitDtor(const ClassInfo& cls_in,
                                   bool out_of_class)
{
	ClassInfo& cls = unit_.classes.Create(cls_in.entity);
	if (cls.implicit_dtor_built || cls.has_user_dtor)
		return;
	cls.implicit_dtor_built = true;
	const string& base_name = cls.members->name;
	Scope* fn_scope = model_.CreateScope(SCOPE_FUNCTION, "~" + base_name,
	                                     cls.members);
	DeferredBody body;
	body.name = "~" + base_name;
	body.fn_scope = fn_scope;
	body.declaring = cls.members;
	body.cls = &cls;
	body.out_of_class = out_of_class;
	body.composed.type = MakeFunctionType(MakeFundamentalType(FT_VOID),
	                                      vector<TypePtr>(), false);
	SemNodePtr item = BuildFunctionNode(body, SF_DESTRUCTOR);
	item->synthesized = true;
	SemNode* node = item.get();

	Scope* saved_scope = current_;
	MethodContext saved_method = method_;
	current_ = fn_scope;
	method_ = MethodContext();
	method_.cls = &cls;
	method_.fn_scope = fn_scope;
	method_.fn_owner = cls.members;
	method_.fn_name = body.name;
	method_.this_type = node->type->parameters[0];
	try
	{
		// PA17: the destructor re-stores this class's vpointer before
		// destroying members and bases.
		if (cls.is_polymorphic)
			node->children.push_back(MakeVPointerStore(cls));
		AnalyzeDtorEpilogue(cls, *node);
	}
	catch (...)
	{
		current_ = saved_scope;
		method_ = saved_method;
		throw;
	}
	current_ = saved_scope;
	method_ = saved_method;
	bool may_throw = false;
	for (size_t i = 0; i < node->children.size(); i++)
		if (NodeMayThrow(*node->children[i]))
			may_throw = true;
	node->unwind_no = !may_throw;
	cls.implicit_dtor_unwind_no = !may_throw;
	unit_.deferred.push_back(std::move(item));
}

// --- constructor selection ------------------------------------------------

// 13.3.1.4: in copy-initialization the source class's conversion
// functions compete with the destination's converting constructors.
// When no converting constructor is usable, a conversion function
// yielding the destination converts the argument in place; the
// copy/move construction from the converted value finishes the
// initialization (elided for trivial copies by the lowering).
bool SemBinder::TryCopyInitSourceConversion(const ClassInfo& cls,
                                            SemValue& arg)
{
	TypePtr from = arg.type ? RemoveTopCv(arg.type) : TypePtr();
	if (!from || from->kind != TK_CLASS || from->named == cls.entity)
		return false;
	TypePtr dest = MakeNamedType(TK_CLASS, cls.entity);
	ImplicitConversion conv;
	try
	{
		conv = ClassifyConversion(MakeConversionSource(arg), dest);
	}
	catch (const std::exception&)
	{
		return false;
	}
	if (!conv.viable || conv.conv_index < 0 || !conv.conv_class)
		return false;
	analyzer_.ApplyConversion(arg, conv, dest);
	return true;
}

int SemBinder::ResolveClassConstructor(const ClassInfo& cls,
                                       vector<SemValue>& args,
                                       bool copy_init, const char* what)
{
	if (cls.ctors.empty() && cls.ctor_templates.empty())
	{
		if (!args.empty())
			throw NoViableOverloadError(
				string("no matching constructor for ") + what);
		return -1;
	}
	vector<TypePtr> candidates;
	vector<size_t> min_arity;
	vector<size_t> positions;
	vector<bool> is_template;
	bool any_default_form = false;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		if (cls.ctors[i].ignore_in_overload ||
		    cls.ctors[i].tmpl_spec)
			continue;
		size_t required = cls.ctors[i].type->parameters.size();
		const vector<const AstExpr*>& defaults = cls.ctors[i].defaults;
		while (required > 0 && required <= defaults.size() &&
		       defaults[required - 1])
			required--;
		if (required == 0)
			any_default_form = true;
		candidates.push_back(cls.ctors[i].type);
		min_arity.push_back(required);
		positions.push_back(i);
		is_template.push_back(false);
	}
	// PA21: constructor templates deduce against the argument list and
	// join as candidates (synthesizing their ClassCtor entries).
	AppendCtorTemplateCandidates(cls, args, candidates, min_arity,
	                             positions, &is_template);
	if (candidates.empty())
	{
		if (!args.empty())
			throw NoViableOverloadError(
				string("no matching constructor for ") + what);
		return -1;
	}
	// Default-initialization of a class whose only constructor entries
	// are the implicitly declared copy/move members uses the implicit
	// default constructor.
	if (args.empty() && !cls.has_user_ctor && !any_default_form)
		return -1;
	vector<ConversionSource> sources;
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	vector<ImplicitConversion> conversions;
	vector<const FunctionSpecialization*> specs(candidates.size(), 0);
	for (size_t c = 0; c < positions.size(); c++)
		specs[c] = cls.ctors[positions[c]].tmpl_spec;
	SpecOverloadOrder order(*this, specs, args.size());
	size_t winner;
	try
	{
		winner = positions[SelectBestOverload(
			candidates, sources, conversions, &min_arity, &is_template,
			&order)];
	}
	catch (const NoViableOverloadError&)
	{
		if (copy_init && args.size() == 1 &&
		    TryCopyInitSourceConversion(cls, args[0]))
			return ResolveClassConstructor(cls, args, copy_init, what);
		throw NoViableOverloadError(
			"no matching constructor for " + cls.entity->display);
	}
	// PA21: a selected constructor-template specialization's body
	// instantiates at this first use (which may append further ctor
	// entries; re-fetch the winner after).
	if (cls.ctors[winner].tmpl_spec)
		InstantiateCtorTemplateBody(const_cast<ClassInfo&>(cls),
		                            (int)winner);
	const ClassCtor& ctor = cls.ctors[winner];
	if (ctor.deleted)
		throw runtime_error(string("use of deleted constructor for ") +
		                    what);
	if (copy_init && ctor.is_explicit)
	{
		// 13.3.1.4: an explicit constructor is not a candidate here;
		// the source class's conversion functions still are.
		if (args.size() == 1 && TryCopyInitSourceConversion(cls, args[0]))
			return ResolveClassConstructor(cls, args, copy_init, what);
		throw runtime_error(string("explicit constructor selected by "
		                           "copy-initialization for ") + what);
	}
	CheckMemberAccess(cls.members, ctor.access, "constructor");
	const TypePtr& fn = ctor.type;
	for (size_t i = 0; i < args.size(); i++)
		if (i < fn->parameters.size())
			analyzer_.ApplyConversion(args[i], conversions[i],
			                          fn->parameters[i]);
	for (size_t i = args.size(); i < fn->parameters.size(); i++)
	{
		// 8.3.6p9/p5: a member default argument's names resolve in the
		// class scope (which chains through a specialization's
		// template-argument aliases), not the call site.
		Scope* saved = current_;
		current_ = cls.members;
		SemValue filled;
		try
		{
			filled = analyzer_.Analyze(*ctor.defaults[i]);
		}
		catch (...)
		{
			current_ = saved;
			throw;
		}
		current_ = saved;
		analyzer_.CopyInitialize(filled, fn->parameters[i],
		                         "default argument");
		args.push_back(std::move(filled));
	}
	if (ctor.defaulted && fn->parameters.empty())
		return -1;
	return (int)winner;
}

SemNodePtr SemBinder::MakeDestructorCall(const ClassInfo& cls,
                                         bool base_entry,
                                         SemNodePtr address)
{
	if (!cls.has_user_dtor)
		EnsureImplicitDtor(cls);
	if (cls.dtor_deleted)
		throw runtime_error("use of deleted destructor");
	CheckMemberAccess(cls.members, cls.dtor_access, "destructor");
	TypePtr dtor_type = MethodAdjustedType(
		cls, MakeFunctionType(MakeFundamentalType(FT_VOID),
		                      vector<TypePtr>(), false));
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::~" + base_name;
	SemNodePtr action = MakeSemNode(SN_DESTRUCTOR_ACTION);
	action->name = qualified;
	action->special = base_entry ? SF_DESTRUCTOR_BASE : SF_DESTRUCTOR;
	SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
	call->type = MakeFundamentalType(FT_VOID);
	call->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = qualified;
	callee->type = dtor_type;
	callee->entity_scope = cls.members;
	callee->entity_name = "~" + base_name;
	callee->is_method = true;
	callee->special = action->special;
	callee->unwind_no = cls.has_user_dtor ? cls.dtor_unwind_no
	                                      : cls.implicit_dtor_unwind_no;
	callee->noexcept_decl = callee->unwind_no;
	call->children.push_back(std::move(callee));
	if (address)
		call->children.push_back(std::move(address));
	action->children.push_back(std::move(call));
	return action;
}
