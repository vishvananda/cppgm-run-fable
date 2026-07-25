#include "sema/sem_binder.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

// PA15 object lifetime: declared-object initialization forms
// (direct, copy, value, aggregate), array element construction, and
// the synthesized field-wise aggregate constructor (8.5, 8.5.1, 12.2).

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA15 assignment boundary");
}

// 13.1p2 parameter-list agreement of an out-of-class declarator with
// an in-class declaration.
bool SameAssignmentSignature(const Type& a, const Type& b)
{
	if (a.parameters.size() != b.parameters.size() ||
	    a.is_const != b.is_const || a.is_volatile != b.is_volatile)
		return false;
	for (size_t i = 0; i < a.parameters.size(); i++)
		if (!TypeEquals(a.parameters[i], b.parameters[i]))
			return false;
	return true;
}

// PA34 designated element `.name = value`: the value binds only to
// its named field; `hole` reports a mismatch (the field
// value-initializes and the item stays for a later field).
const AstExpr* DesignatedFieldElement(const AstExpr* element,
                                      const ClassField& field,
                                      bool& hole)
{
	hole = false;
	if (!element || element->kind != EK_DESIGNATED)
		return element;
	if (element->name.parts[0].identifier == field.name)
		return element->operands[0].get();
	hole = true;
	return 0;
}

const AstExpr* DesignatedFieldElement(const AstExpr* element,
                                      const ClassField& field)
{
	bool hole = false;
	return DesignatedFieldElement(element, field, hole);
}

}  // namespace

// --- object lifetime --------------------------------------------------------

SemNodePtr SemBinder::VariableObjectExpr(const ScopeBinding& binding)
{
	SemNodePtr object = MakeSemNode(SN_ID_EXPRESSION);
	object->name = binding.name;
	object->type = binding.type;
	object->category = VC_LVALUE;
	object->entity_scope = binding.home;
	object->entity_name = binding.name;
	return object;
}

// 12.2p5: a class temporary bound directly to a local reference
// declaration persists for the reference's lifetime. The bound
// prvalue drops its full-expression cleanup (the elision pattern of
// AppendElidedObjectInit) and the declaration destroys the referee at
// scope exit through the stored reference instead.
void SemBinder::ExtendBoundTemporaryLifetime(SemNode& item,
                                             const ScopeBinding& binding)
{
	if (!IsReferenceType(binding.type) || item.children.empty())
		return;
	// Namespace-scope extension (destruction at shutdown) stays out of
	// scope: the reference emission has no static cleanup for it.
	if (!binding.home || (binding.home->kind != SCOPE_BLOCK &&
	                      binding.home->kind != SCOPE_FUNCTION))
		return;
	SemNode* bound = item.children.back().get();
	if (!bound || !bound->needs_dtor || bound->category != VC_PRVALUE)
		return;
	TypePtr bare = bound->type ? RemoveTopCv(bound->type) : TypePtr();
	if (!bare || bare->kind != TK_CLASS)
		return;
	const ClassInfo* cls = unit_.classes.Find(bare->named);
	if (!cls || !unit_.classes.NeedsDestruction(*cls))
		return;
	// The bound node keeps needs_dtor (it drives the result
	// materialization); the flag reroutes its cleanup registration.
	bound->lifetime_extended = true;
	item.needs_dtor = true;
	SemNodePtr object = VariableObjectExpr(binding);
	// The reference collapses on use: the destructor receives the
	// referee's address (a pointer to a reference is ill-formed).
	object->type = binding.type->target;
	item.children.push_back(MakeDestructorCall(
		*cls, false, AddressOfNode(std::move(object))));
}

void SemBinder::AppendAggregateInit(const ClassInfo& cls,
                                    const SemNode& target_proto,
                                    const AstExpr& braced,
                                    vector<SemNodePtr>& out)
{
	bf_units_written_.clear();
	size_t used = ConsumeAggregateItems(cls, target_proto,
	                                    braced.arguments, 0, true, out);
	if (used < braced.arguments.size())
		throw runtime_error("too many initializers for aggregate");
}

// A work-free member-class temporary retargeted onto the member
// itself (copy elision): a declared-default constructor stays odr
// -used (elided), a fully implicit one drops (trivial_init).
SemNodePtr SemBinder::ElideMemberTemporary(const ClassInfo& member_cls,
                                           SemNodePtr action,
                                           SemNodePtr member)
{
	SemNode& call = *action->children[0];
	bool declared_default = false;
	for (size_t i = 0; i < member_cls.ctors.size(); i++)
		if (member_cls.ctors[i].kind == CK_ORDINARY &&
		    !member_cls.ctors[i].implicit &&
		    member_cls.ctors[i].type->parameters.empty())
			declared_default = true;
	if (declared_default)
		action->elided = true;
	else
		action->trivial_init = true;
	call.children.insert(call.children.begin() + 1,
	                     AddressOfNode(std::move(member)));
	action->ctor_addressed = true;
	return action;
}

// 8.5.1 with brace elision: members initialize in declaration order
// from `items`, a nested braced item initializes one subaggregate
// fully, and a non-braced item starts an elided subaggregate that
// One class-type member inside an aggregate: its own braces
// initialize it fully (aggregate recursion or list-construction);
// otherwise an elided subaggregate or one copy-initializing item.
size_t SemBinder::ConsumeAggregateClassItem(const ClassInfo& member_cls,
                                            const ClassField& field,
                                            SemNodePtr member,
                                            const vector<AstExprPtr>& items,
                                            size_t at,
                                            vector<SemNodePtr>& out)
{
	const AstExpr* element = at < items.size() ? items[at].get() : 0;
	bool designated_hole = false;
	element = DesignatedFieldElement(element, field, designated_hole);
	if (element && element->kind == EK_BRACED)
	{
		at++;
		if (member_cls.is_aggregate)
		{
			size_t used = ConsumeAggregateItems(
				member_cls, *member, element->arguments, 0,
				true, out);
			if (used < element->arguments.size())
				throw runtime_error("too many initializers "
				                    "for aggregate member");
		}
		else
		{
			vector<SemValue> values;
			for (size_t j = 0;
			     j < element->arguments.size(); j++)
				values.push_back(analyzer_.Analyze(
					*element->arguments[j]));
			int index = ResolveClassConstructor(
				member_cls, values, true, field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			for (size_t j = 0; j < values.size(); j++)
				arg_nodes.push_back(std::move(values[j].node));
			out.push_back(MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(std::move(member)),
				std::move(arg_nodes)));
		}
		return at;
	}
	if (element && member_cls.is_aggregate)
	{
		// 8.5.1p13: an expression convertible to the subaggregate's
		// type initializes the whole member; only otherwise does brace
		// elision consume items element-wise.
		SemValue probe = analyzer_.Analyze(*element);
		TypePtr probe_type = RemoveTopCv(probe.type);
		// A work-free temporary of the member's own class elides into
		// the member (the named-object elision above): no copy, no
		// explicit value-fill (the pinned reference shape).
		if (probe.node && probe.node->kind == SN_CONSTRUCTOR_ACTION &&
		    probe_type->kind == TK_CLASS &&
		    probe_type->named == member_cls.entity &&
		    !probe.node->children.empty() &&
		    probe.node->children[0]->children.size() == 1 &&
		    !member_cls.has_user_ctor &&
		    !unit_.classes.NeedsConstruction(member_cls))
		{
			out.push_back(ElideMemberTemporary(
				member_cls, std::move(probe.node), std::move(member)));
			return at + 1;
		}
		if (probe_type->kind == TK_CLASS &&
		    BaseClassDistance(probe_type->named, member_cls.entity) >= 0)
		{
			vector<SemValue> values;
			values.push_back(std::move(probe));
			int index = ResolveClassConstructor(member_cls, values, true,
			                                    field.name.c_str());
			vector<SemNodePtr> arg_nodes;
			arg_nodes.push_back(std::move(values[0].node));
			out.push_back(MakeConstructorCall(
				member_cls, index, false,
				AddressOfNode(std::move(member)),
				std::move(arg_nodes)));
			return at + 1;
		}
	}
	if (member_cls.is_aggregate)
	{
		// Brace elision consumes the following items in place; a
		// designated hole consumes nothing (the member value
		// -initializes field-wise, its item staying for a later field).
		static const vector<AstExprPtr> kNoItems;
		size_t used = ConsumeAggregateItems(
			member_cls, *member, designated_hole ? kNoItems : items,
			designated_hole ? 0 : at, false, out);
		return designated_hole ? at : used;
	}
	// A non-aggregate member without its own braces: one item
	// copy-initializes it (or it default-constructs).
	vector<SemValue> values;
	if (element)
	{
		at++;
		values.push_back(analyzer_.Analyze(*element));
	}
	if (!values.empty() && values[0].node &&
	    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == member_cls.entity)
	{
		SemNodePtr action = std::move(values[0].node);
		SemNode& call = *action->children[0];
		call.children.insert(
			call.children.begin() + 1,
			AddressOfNode(std::move(member)));
		action->ctor_addressed = true;
		out.push_back(std::move(action));
		return at;
	}
	int index = ResolveClassConstructor(member_cls, values,
	                                    true,
	                                    field.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < values.size(); j++)
		arg_nodes.push_back(std::move(values[j].node));
	out.push_back(MakeConstructorCall(
		member_cls, index, false,
		AddressOfNode(std::move(member)),
		std::move(arg_nodes)));
	return at;
}

// consumes following items. Returns the next unused position.
size_t SemBinder::ConsumeAggregateItems(const ClassInfo& cls,
                                        const SemNode& target_proto,
                                        const vector<AstExprPtr>& items,
                                        size_t at, bool top_braced,
                                        vector<SemNodePtr>& out)
{
	// `out` accumulates across nesting levels, so a union's one-member
	// rule counts only the actions this call appended.
	size_t entry_actions = out.size();
	for (size_t i = 0; i < cls.fields.size(); i++)
	{
		const ClassField& field = cls.fields[i];
		if (field.name.empty())
			continue;
		// 8.5.1p15: a braced union initializer initializes the first
		// non-static data member only.
		if (cls.is_union && out.size() > entry_actions)
			break;
		// PA34: `{.m = v}` over a union selects the named member
		// instead of the first.
		if (cls.is_union && at < items.size() &&
		    items[at]->kind == EK_DESIGNATED &&
		    items[at]->name.parts[0].identifier != field.name)
			continue;
		SemNodePtr member = MakeSemNode(SN_MEMBER_EXPRESSION);
		member->name = field.name;
		member->type = IsReferenceType(field.type) ? field.type->target
		                                           : field.type;
		member->member_ref = IsReferenceType(field.type);
		member->category = VC_LVALUE;
		member->member_offset = field.offset;
		member->is_bit_field = field.is_bit_field;
		member->bit_offset = field.bit_offset;
		member->bit_width = field.bit_width;
		member->children.push_back(CloneSemNode(target_proto));
		const AstExpr* element = at < items.size() ? items[at].get() : 0;
		element = DesignatedFieldElement(element, field);
		TypePtr bare = RemoveTopCv(field.type);
		const ClassInfo* member_cls = bare->kind == TK_CLASS
			? unit_.classes.Find(bare->named) : 0;
		if (member_cls)
		{
			at = ConsumeAggregateClassItem(*member_cls, field,
			                               std::move(member), items, at,
			                               out);
			continue;
		}
		if (bare->kind == TK_ARRAY)
		{
			at = ConsumeArrayItems(field, *member, items, at,
			                       top_braced, out);
			continue;
		}
		ClassField row = field;
		if (element)
		{
			at++;
			SemValue value = analyzer_.Analyze(*element);
			if (!IsReferenceType(field.type))
				CheckListInitNarrowing(value, bare);
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 std::move(value)));
		}
		else
		{
			if (IsReferenceType(field.type))
				throw runtime_error("reference member is not "
				                    "initialized");
			out.push_back(MemberAssignAction(row, std::move(member),
			                                 ZeroValue(bare)));
		}
	}
	return at;
}

// One array member inside an aggregate: a braced item initializes it
// fully; otherwise the elements consume following items (elision).
size_t SemBinder::ConsumeArrayItems(const ClassField& field,
                                    const SemNode& member_proto,
                                    const vector<AstExprPtr>& items,
                                    size_t at, bool top_braced,
                                    vector<SemNodePtr>& out)
{
	TypePtr array = RemoveTopCv(field.type);
	TypePtr element_type = RemoveTopCv(array->target);
	if (element_type->kind == TK_CLASS)
		throw OutsideBoundary("aggregate class array member");
	const AstExpr* element = at < items.size() ? items[at].get() : 0;
	element = DesignatedFieldElement(element, field);
	if (element && element->kind == EK_LITERAL &&
	    element->literal_kind == PTK_LITERAL_ARRAY &&
	    IsIntegralType(element_type))
	{
		// 8.5.2: a string literal initializes the character array
		// element-wise (trailing elements zero).
		SemValue value = analyzer_.Analyze(*element);
		const string& bytes = value.node->string_bytes;
		unsigned long long width = TypeSize(element_type);
		if (bytes.size() / width > array->bound)
			throw runtime_error("string literal exceeds the array "
			                    "member");
		for (unsigned long long j = 0; j < array->bound; j++)
		{
			unsigned long long code = 0;
			if ((j + 1) * width <= bytes.size())
				code = LittleEndianValue(
					bytes.substr(j * width, width));
			ClassField element_field;
			element_field.name = field.name;
			element_field.type = element_type;
			SemValue item_value;
			item_value.node = MakeSemNode(SN_LITERAL);
			item_value.node->token = std::to_string(code);
			item_value.node->type = element_type;
			item_value.node->category = VC_PRVALUE;
			item_value.node->has_value = true;
			item_value.node->value = ConstValue(
				element_type->fundamental, code);
			item_value.type = element_type;
			out.push_back(MemberAssignAction(
				element_field,
				SubscriptNode(CloneSemNode(member_proto), j),
				std::move(item_value)));
		}
		return at + 1;
	}
	const vector<AstExprPtr>* source = &items;
	size_t inner_at = at;
	bool own_braces = element && element->kind == EK_BRACED;
	if (own_braces)
		source = &element->arguments;
	if (own_braces)
		inner_at = 0;
	for (unsigned long long j = 0; j < array->bound; j++)
	{
		ClassField element_field;
		element_field.name = field.name;
		element_field.type = element_type;
		SemNodePtr target =
			SubscriptNode(CloneSemNode(member_proto), j);
		const AstExpr* item_expr = inner_at < source->size()
			? (*source)[inner_at].get() : 0;
		// PA34: a designated item belongs to a later field, ending an
		// elided element run.
		if (item_expr && !own_braces &&
		    item_expr->kind == EK_DESIGNATED)
			item_expr = 0;
		if (item_expr && (own_braces || inner_at < source->size()))
		{
			if (!own_braces && item_expr->kind == EK_BRACED)
				throw OutsideBoundary("nested braces in an elided "
				                      "array member");
			inner_at++;
			SemValue value = analyzer_.Analyze(*item_expr);
			CheckListInitNarrowing(value, element_type);
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 std::move(value)));
		}
		else
			out.push_back(MemberAssignAction(element_field,
			                                 std::move(target),
			                                 ZeroValue(element_type)));
	}
	if (own_braces)
	{
		if (inner_at < source->size())
			throw runtime_error("too many initializers for array "
			                    "member");
		return at + 1;
	}
	return inner_at;
}

// A braced array of aggregates: each element runs the synthesized
// field-wise constructor at its byte offset (the action records the
// A declared class array: braced per-element construction (aggregate
// classes at block scope take the field-wise form), or the per-element
// default construction of an uninitialized array.
// One class-array element's list-initialization (a null `element`
// value-initializes). Braced local elements share the array base
// address and construct at their byte offset; namespace-scope
// elements keep the subscripted form used by the global-init helper.
void SemBinder::AppendArrayElementInit(SemNode& item,
                                       ScopeBinding& binding,
                                       const ClassInfo& cls,
                                       unsigned long long at,
                                       const AstExpr* element)
{
	bool shared_base = binding.home &&
		binding.home->kind != SCOPE_NAMESPACE;
	vector<SemValue> values;
	if (element && element->kind == EK_BRACED)
		for (size_t j = 0; j < element->arguments.size(); j++)
			values.push_back(analyzer_.Analyze(
				*element->arguments[j]));
	else if (element)
		values.push_back(analyzer_.Analyze(*element));
	if (values.size() == 1 && values[0].node &&
	    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
	    values[0].category == VC_PRVALUE &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == cls.entity)
	{
		// 12.8p31: the element temporary elides; the constructor runs
		// on the element directly.
		SemNodePtr action = std::move(values[0].node);
		action->needs_dtor = false;
		while (action->children.size() > 1 &&
		       action->children.back()->kind == SN_DESTRUCTOR_ACTION)
			action->children.pop_back();
		if (shared_base)
		{
			action->has_value = true;
			action->value = ConstValue(FT_UNSIGNED_LONG_INT,
			                           at * cls.size);
		}
		else
		{
			SemNode& call = *action->children[0];
			call.children.insert(
				call.children.begin() + 1,
				AddressOfNode(SubscriptNode(
					VariableObjectExpr(binding), at)));
			action->ctor_addressed = true;
		}
		item.children.push_back(std::move(action));
		return;
	}
	int index = ResolveClassConstructor(cls, values, element != 0,
	                                    binding.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < values.size(); j++)
		arg_nodes.push_back(std::move(values[j].node));
	SemNodePtr action = MakeConstructorCall(
		cls, index, false,
		shared_base ? SemNodePtr()
		            : AddressOfNode(SubscriptNode(
		                  VariableObjectExpr(binding), at)),
		std::move(arg_nodes));
	if (shared_base)
	{
		action->has_value = true;
		action->value = ConstValue(FT_UNSIGNED_LONG_INT,
		                           at * cls.size);
	}
	item.children.push_back(std::move(action));
}

void SemBinder::AppendClassArrayInit(SemNode& item, ScopeBinding& binding,
                                     const AstInitializer* init,
                                     const ClassInfo& cls)
{
	TypePtr type = binding.type;

	if (init)
	{
		const AstExpr* braced = 0;
		if (init->kind == INIT_BRACED)
			braced = init->expr.get();
		else if (init->kind == INIT_EQ &&
		         init->expr->kind == EK_BRACED)
			braced = init->expr.get();
		if (!braced)
			throw OutsideBoundary("class array initializer form");
		if (!cls.has_user_ctor && cls.is_aggregate &&
		    binding.home && binding.home->kind != SCOPE_NAMESPACE)
		{
			AppendAggregateArrayInit(item, binding, cls, *braced);
			return;
		}
		// Elements list-initialize through their constructors at
		// subscripted addresses.
		TypePtr completed = binding.type;
		if (!completed->bound_known)
		{
			completed = MakeArrayType(completed->target, true,
			                          braced->arguments.size());
			binding.type = completed;
			item.type = completed;
		}
		if (braced->arguments.size() > completed->bound)
			throw runtime_error("too many initializers for " +
			                    binding.name);
		for (size_t i = 0; i < braced->arguments.size(); i++)
			AppendArrayElementInit(item, binding, cls, i,
			                       braced->arguments[i].get());
		// 8.5.1p7: elements beyond the initializer list
		// value-initialize.
		for (unsigned long long i = braced->arguments.size();
		     i < completed->bound; i++)
			AppendArrayElementInit(item, binding, cls, i, 0);
		return;
	}
	if (!cls.has_user_ctor && !unit_.classes.NeedsConstruction(cls))
		return;
	for (unsigned long long i = 0; i < type->bound; i++)
	{
		vector<SemValue> no_args;
		int index = ResolveClassConstructor(cls, no_args, false,
		                                    binding.name.c_str());
		vector<SemNodePtr> arg_nodes;
		for (size_t j = 0; j < no_args.size(); j++)
			arg_nodes.push_back(std::move(no_args[j].node));
		item.children.push_back(MakeConstructorCall(
			cls, index, false,
			AddressOfNode(SubscriptNode(VariableObjectExpr(binding),
			                            i)),
			std::move(arg_nodes)));
	}
	return;
}

// offset; the lowering shares one base address).
void SemBinder::AppendAggregateArrayInit(SemNode& item,
                                         ScopeBinding& binding,
                                         const ClassInfo& cls,
                                         const AstExpr& braced)
{
	TypePtr array = binding.type;
	if (!array->bound_known)
	{
		array = MakeArrayType(array->target, true,
		                      braced.arguments.size());
		binding.type = array;
		item.type = array;
	}
	if (braced.arguments.size() > array->bound)
		throw runtime_error("too many initializers for " + binding.name);
	// Array elements share one full-cover signature; omitted element
	// members pad with zero arguments at the call sites.
	TypePtr adjusted = EnsureAggregateCtor(cls, (size_t)-1);
	unsigned long long size = cls.size;
	const string& base_name = cls.members->name;
	string qualified = QualifiedScopePath(cls.members->parent) +
		base_name + "::" + base_name;
	for (size_t i = 0; i < braced.arguments.size(); i++)
	{
		const AstExpr* element = braced.arguments[i].get();
		if (element->kind != EK_BRACED)
			throw OutsideBoundary("array aggregate element form");
		vector<SemValue> values;
		size_t value_at = 0;
		SemNodePtr action = MakeSemNode(SN_CONSTRUCTOR_ACTION);
		action->name = qualified;
		action->special = SF_CONSTRUCTOR;
		action->has_value = true;
		action->value = ConstValue(FT_UNSIGNED_LONG_INT, i * size);
		SemNodePtr call = MakeSemNode(SN_CALL_EXPRESSION);
		call->type = MakeFundamentalType(FT_VOID);
		call->category = VC_PRVALUE;
		SemNodePtr callee = MakeSemNode(SN_CALLEE);
		callee->name = qualified;
		callee->type = adjusted;
		callee->entity_scope = cls.members;
		callee->entity_name = base_name;
		callee->is_method = true;
		callee->special = SF_CONSTRUCTOR;
		callee->unwind_no = true;
		call->children.push_back(std::move(callee));
		for (size_t j = 0; j < element->arguments.size(); j++)
		{
			SemValue value = analyzer_.Analyze(*element->arguments[j]);
			if (value_at + 1 < adjusted->parameters.size())
				analyzer_.CopyInitialize(
					value,
					adjusted->parameters[value_at + 1],
					"aggregate element");
			value_at++;
			call->children.push_back(std::move(value.node));
		}
		if (element->arguments.size() + 1 > adjusted->parameters.size())
			throw runtime_error("too many initializers for array "
			                    "element");
		// Trailing fields value-initialize through zero arguments.
		for (size_t j = element->arguments.size() + 1;
		     j < adjusted->parameters.size(); j++)
		{
			TypePtr param = adjusted->parameters[j];
			SemValue zero = ZeroValue(
				IsReferenceType(param) ? param->target
				                       : RemoveTopCv(param));
			call->children.push_back(std::move(zero.node));
		}
		action->children.push_back(std::move(call));
		item.children.push_back(std::move(action));
	}
}

void SemBinder::AttachObjectLifetime(SemNode& item, ScopeBinding& binding,
                                     const AstInitializer* init,
                                     const DeclSpecifierInfo& specs)
{
	TypePtr type = binding.type;
	TypePtr inner = type;
	while (inner->kind == TK_ARRAY)
		inner = inner->target;
	inner = RemoveTopCv(inner);
	bool class_object = !IsReferenceType(type) &&
		inner->kind == TK_CLASS;
	if (!class_object)
	{
		if (init)
		{
			AnalyzeVariableInit(item, binding, init);
			ExtendBoundTemporaryLifetime(item, binding);
		}
		return;
	}
	if (specs.is_extern && !init)
		return;  // a declaration of an object defined elsewhere
	if (!inner->named->complete)
		throw runtime_error(inner->named->display +
		                    " is an incomplete type");
	const ClassInfo* cls = unit_.classes.Find(inner->named);
	if (!cls)
		throw runtime_error("class record missing for " +
		                    inner->named->display);
	AppendClassObjectInit(item, binding, init, *cls);
	// Destruction at scope exit / program shutdown.
	bool needs_dtor = unit_.classes.NeedsDestruction(*cls);
	if (!needs_dtor)
		return;
	if (!unit_.classes.DestructionHasEffects(*cls))
	{
		// The destructor is still potentially invoked (resolved and
		// access-checked); an effect-free chain emits no cleanup, but
		// the selected destructor stays odr-used (3.2p3).
		unit_.elided_dtor_uses.push_back(
			MakeDestructorCall(*cls, false, SemNodePtr()));
		return;
	}
	item.needs_dtor = true;
	if (type->kind == TK_ARRAY)
	{
		for (unsigned long long i = 0; i < type->bound; i++)
			item.children.push_back(MakeDestructorCall(
				*cls, false,
				AddressOfNode(SubscriptNode(VariableObjectExpr(binding),
				                            i))));
		return;
	}
	item.children.push_back(MakeDestructorCall(
		*cls, false, AddressOfNode(VariableObjectExpr(binding))));
}

// 12.8p31: the same-class temporary elides; the constructor runs
// directly on the declared object.
void SemBinder::AppendElidedObjectInit(SemNode& item,
                                       ScopeBinding& binding,
                                       const ClassInfo& cls,
                                       SemNodePtr action)
{
	// The elided temporary's own cleanup does not apply; the declared
	// object's lifetime governs.
	action->needs_dtor = false;
	while (action->children.size() > 1 &&
	       action->children.back()->kind == SN_DESTRUCTOR_ACTION)
		action->children.pop_back();
	SemNode& call = *action->children[0];
	// The explicit-temporary marking does not survive elision into a
	// namespace-scope object: with no observable construction work the
	// reference emits no dynamic initializer for it. A user-declared
	// (defaulted) default constructor selected for the elided init is
	// still odr-used (3.2p3): its synthesized body emits, call-free.
	if (binding.owner &&
	    (binding.owner->kind == SCOPE_NAMESPACE ||
	     binding.owner->kind == SCOPE_BLOCK ||
	     binding.owner->kind == SCOPE_FUNCTION) &&
	    call.children.size() == 1 && !cls.has_user_ctor &&
	    !unit_.classes.NeedsConstruction(cls))
	{
		bool declared_default = false;
		for (size_t i = 0; i < cls.ctors.size(); i++)
			if (cls.ctors[i].kind == CK_ORDINARY &&
			    !cls.ctors[i].implicit &&
			    cls.ctors[i].type->parameters.empty())
				declared_default = true;
		if (declared_default)
			action->elided = true;
		else
			action->trivial_init = true;
	}
	call.children.insert(call.children.begin() + 1,
	                     AddressOfNode(VariableObjectExpr(binding)));
	action->ctor_addressed = true;
	item.children.push_back(std::move(action));
}

// The initializer-form split of one class-object initializer: paren
// arguments, a braced list, or a copy-initializing expression.
// Returns whether the form copy-initializes.
bool SemBinder::ClassifyClassInitForm(const AstInitializer& init,
                                      vector<const AstExpr*>& args,
                                      const AstExpr*& braced)
{
	switch (init.kind)
	{
	case INIT_PAREN:
		for (size_t i = 0; i < init.args.size(); i++)
			args.push_back(init.args[i].get());
		return false;
	case INIT_EQ:
		if (init.expr->kind == EK_BRACED)
			braced = init.expr.get();
		else
			args.push_back(init.expr.get());
		return true;
	case INIT_BRACED:
		braced = init.expr.get();
		return false;
	default:
		throw OutsideBoundary("class initializer form");
	}
}

// The analyzed initializer arguments; a pack expansion contributes
// one value per element.
void SemBinder::AnalyzeInitArguments(const vector<const AstExpr*>& items,
                                     vector<SemValue>& values)
{
	for (size_t i = 0; i < items.size(); i++)
	{
		if (items[i]->kind == EK_PACK_EXPANSION)
		{
			if (!ExpandPackExpression(*items[i]->operands[0], values))
				throw runtime_error("pack expansion outside an "
				                    "expandable context");
			continue;
		}
		if (items[i]->kind == EK_BRACED)
		{
			// 8.5.4: the list defers; overload resolution picks the
			// parameter it initializes.
			SemValue value;
			value.braced_list = true;
			analyzer_.AnalyzeArgumentList(items[i]->arguments,
			                              value.list_values);
			values.push_back(std::move(value));
			continue;
		}
		values.push_back(analyzer_.Analyze(*items[i]));
	}
}

// PA25 8.5.4p3: a braced initializer over a std::initializer_list
// object fills the {begin, size} record from the elements; returns
// whether the initializer took this form.
bool SemBinder::AppendInitializerListObjectInit(SemNode& item,
                                                ScopeBinding& binding,
                                                const AstInitializer& init)
{
	TypePtr list_element;
	if (!IsStdInitializerList(binding.type, &list_element))
		return false;
	const AstExpr* braced = 0;
	if (init.kind == INIT_BRACED)
		braced = init.expr.get();
	else if (init.kind == INIT_EQ && init.expr->kind == EK_BRACED)
		braced = init.expr.get();
	if (!braced)
		return false;
	TypePtr element = RemoveTopCv(list_element);
	vector<SemValue> elements;
	analyzer_.AnalyzeArgumentList(braced->arguments, elements);
	SemNodePtr list = MakeSemNode(SN_BRACED_INIT_LIST);
	list->type = RemoveTopCv(IsReferenceType(binding.type)
	                             ? binding.type->target : binding.type);
	list->category = VC_PRVALUE;
	for (size_t i = 0; i < elements.size(); i++)
	{
		analyzer_.CopyInitialize(elements[i], element,
		                         "initializer-list element");
		list->children.push_back(std::move(elements[i].node));
	}
	item.children.push_back(std::move(list));
	return true;
}

// Default-initialization. The PA12 dump pins the constructor-action
// plus synthesized empty definition shape for the trivial subset;
// richer classes resolve a real constructor.
void SemBinder::AppendClassDefaultInit(SemNode& item,
                                       ScopeBinding& binding,
                                       const ClassInfo& cls)
{
	if (!cls.has_user_ctor)
	{
		TypePtr ctor_type;
		EnsureDefaultConstructor(RemoveTopCv(binding.type), ctor_type);
	}
	vector<SemValue> no_args;
	int index = ResolveClassConstructor(cls, no_args, false,
	                                    binding.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t j = 0; j < no_args.size(); j++)
		arg_nodes.push_back(std::move(no_args[j].node));
	SemNodePtr action = MakeConstructorCall(
		cls, index, false, AddressOfNode(VariableObjectExpr(binding)),
		std::move(arg_nodes));
	// An implicitly-declared default constructor over an effect-free
	// chain (empty user default-constructor bodies in the members
	// included) elides its call; the selected constructor stays
	// odr-used. A user-provided constructor selected directly keeps
	// its explicit call (the reference pins both shapes).
	bool implicit_selected = index < 0 ||
		(index < (int)cls.ctors.size() &&
		 (cls.ctors[index].implicit || cls.ctors[index].defaulted));
	if (implicit_selected &&
	    !unit_.classes.DefaultConstructionHasSyntacticEffects(cls))
		action->elided = true;
	item.children.push_back(std::move(action));
}

// PA36 13.3.1.7 first phase: initializer-list constructors take
// the whole braced list before any flattened element pass; the
// materialized list (defaults filled) lands in out_args.
int SemBinder::SelectListCtorInit(const ClassInfo& cls,
                                  const AstExpr& braced,
                                  vector<SemValue>& out_args)
{
	int best = -1;
	ImplicitConversion best_conv;
	SemValue list_value;
	bool analyzed = false;
	for (size_t i = 0; i < cls.ctors.size(); i++)
	{
		const ClassCtor& ctor = cls.ctors[i];
		if (ctor.ignore_in_overload || ctor.tmpl_spec || ctor.deleted ||
		    ctor.kind != CK_ORDINARY || ctor.type->parameters.empty())
			continue;
		const vector<TypePtr>& params = ctor.type->parameters;
		bool tail_defaulted = true;
		for (size_t j = 1; tail_defaulted && j < params.size(); j++)
			if (j >= ctor.defaults.size() || !ctor.defaults[j])
				tail_defaulted = false;
		if (!tail_defaulted)
			continue;
		TypePtr first = params[0];
		if (IsReferenceType(first))
			first = first->target;
		if (!IsStdInitializerList(RemoveTopCv(first), 0))
			continue;
		// The elements analyze once, on the first list-constructor
		// candidate (a class without one skips the whole phase).
		if (!analyzed)
		{
			analyzed = true;
			list_value.braced_list = true;
			vector<const AstExpr*> items;
			for (size_t a = 0; a < braced.arguments.size(); a++)
				items.push_back(braced.arguments[a].get());
			AnalyzeInitArguments(items, list_value.list_values);
		}
		ImplicitConversion conv = ClassifyConversionEx(
			MakeConversionSource(list_value), params[0], false);
		if (!conv.viable)
			continue;
		if (best >= 0)
		{
			if (conv.rank == best_conv.rank)
				throw runtime_error(
					"ambiguous initializer-list constructor for " +
					cls.entity->display);
			if (conv.rank > best_conv.rank)
				continue;
		}
		best = (int)i;
		best_conv = conv;
	}
	if (best < 0)
		return -1;
	analyzer_.ApplyConversion(list_value, best_conv,
	                          cls.ctors[best].type->parameters[0]);
	out_args.push_back(std::move(list_value));
	FillCtorDefaultArguments(cls, cls.ctors[best], out_args);
	return best;
}


void SemBinder::AppendClassObjectInit(SemNode& item, ScopeBinding& binding,
                                      const AstInitializer* init,
                                      const ClassInfo& cls)
{
	if (binding.type->kind == TK_ARRAY)
	{
		AppendClassArrayInit(item, binding, init, cls);
		return;
	}
	// PA25 8.5.4p3: a braced initializer over a std::initializer_list
	// object fills the {begin, size} record from the elements.
	if (init && AppendInitializerListObjectInit(item, binding, *init))
		return;
	if (!init)
	{
		AppendClassDefaultInit(item, binding, cls);
		return;
	}
	// Initialized class object: direct (paren), list (braced), or
	// copy-initialization.
	vector<const AstExpr*> args;
	const AstExpr* braced = 0;
	bool copy_init = ClassifyClassInitForm(*init, args, braced);
	if (braced && cls.is_aggregate)
	{
		SemNodePtr proto = VariableObjectExpr(binding);
		vector<SemNodePtr> actions;
		bool has_pack_item = false;
		for (size_t i = 0; i < braced->arguments.size(); i++)
			if (braced->arguments[i]->kind == EK_PACK_EXPANSION)
				has_pack_item = true;
		if (has_pack_item)
			AppendExpandedAggregateInit(cls, *proto, *braced, actions);
		else
			AppendAggregateInit(cls, *proto, *braced, actions);
		for (size_t i = 0; i < actions.size(); i++)
			item.children.push_back(std::move(actions[i]));
		return;
	}
	vector<SemValue> values;
	int index = -1;
	bool list_ctor = false;
	if (braced)
	{
		// PA36 13.3.1.7: an initializer-list constructor takes the
		// whole braced list first; the flattened element pass runs
		// only when none is viable.
		index = SelectListCtorInit(cls, *braced, values);
		if (index >= 0)
			list_ctor = true;
		else
		{
			vector<const AstExpr*> items;
			for (size_t i = 0; i < braced->arguments.size(); i++)
				items.push_back(braced->arguments[i].get());
			AnalyzeInitArguments(items, values);
		}
	}
	else
		AnalyzeInitArguments(args, values);
	if (copy_init && !braced && values.size() == 1 &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    BaseClassDistance(RemoveTopCv(values[0].type)->named,
	                      cls.entity) >= 0)
		// PA16 copy-initialization from a class value: the copy/move
		// constructor wraps the source (unless the source already
		// constructs in place); the elision below targets the object.
		analyzer_.CopyInitialize(values[0],
		                         MakeNamedType(TK_CLASS, cls.entity),
		                         "initialization");
	if (!list_ctor && values.size() == 1 && values[0].node &&
	    values[0].node->kind == SN_CONSTRUCTOR_ACTION &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == cls.entity)
	{
		AppendElidedObjectInit(item, binding, cls,
		                       std::move(values[0].node));
		return;
	}
	if (!list_ctor && values.size() == 1 && values[0].node &&
	    values[0].category == VC_PRVALUE &&
	    RemoveTopCv(values[0].type)->kind == TK_CLASS &&
	    RemoveTopCv(values[0].type)->named == cls.entity)
	{
		// A same-class prvalue initializer (call result, conditional)
		// constructs the declared object directly (copy elision).
		item.children.push_back(std::move(values[0].node));
		return;
	}
	if (!list_ctor)
		index = ResolveClassConstructor(cls, values, copy_init,
		                                binding.name.c_str());
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < values.size(); i++)
		arg_nodes.push_back(std::move(values[i].node));
	SemNodePtr action = MakeConstructorCall(
		cls, index, false, AddressOfNode(VariableObjectExpr(binding)),
		std::move(arg_nodes));
	// 8.5.4: braced value-initialization of an instantiated
	// specialization spells its (synthesized) constructor call even
	// when trivial - the reference pins the demanded C1 for these
	// classes (ordinary classes keep the elided shape).
	if (braced && values.empty() && cls.entity &&
	    cls.entity->spec_template && !cls.entity->is_template_anchor)
		action->trivial_init = false;
	item.children.push_back(std::move(action));
}

void SemBinder::CheckQualifiedDefinitionScope(const Scope* declaring)
{
	if (declaring->kind != SCOPE_NAMESPACE &&
	    declaring->kind != SCOPE_CLASS)
		throw OutsideBoundary("qualified function definition scope");
}

// A qualified declarator at namespace scope: the out-of-class
// definition of a static data member (9.4.2p2).
void SemBinder::BindQualifiedDeclarator(const DeclSpecifierInfo& specs,
                                        const AstInitDeclarator& declarator,
                                        const DeclaratorInfo& composed)
{
	if (composed.type->kind == TK_FUNCTION)
	{
		// PA16: an out-of-class `= default` assignment operator
		// synthesizes its definition as a source-owned strong one.
		string name = DeclaredFunctionName(composed.id->parts.back());
		Scope* owner = ResolvePrefixScope(*composed.id);
		if (owner->kind != SCOPE_CLASS || name != "operator =" ||
		    !declarator.init || declarator.init->kind != INIT_DEFAULT)
			throw OutsideBoundary("qualified function declarator");
		const NamedTypeInfo* entity = model_.ScopeEntity(owner);
		ClassInfo* cls = entity ? unit_.classes.Find(entity) : 0;
		ScopeBinding* binding = FindOwnBinding(*owner, name);
		if (!cls || !binding || binding->kind != SB_FUNCTION)
			throw runtime_error("defaulted operator= matches no "
			                    "declaration");
		size_t count = binding->overloads.size() + 1;
		size_t index = count;
		for (size_t i = 0; i < count; i++)
		{
			const TypePtr& declared =
				i == 0 ? binding->type : binding->overloads[i - 1];
			if (SameAssignmentSignature(*declared, *composed.type))
				index = i;
		}
		if (index == count)
			throw runtime_error("defaulted operator= matches no "
			                    "declaration");
		binding->fn_defaulted.resize(count, false);
		binding->fn_defaulted[index] = true;
		const TypePtr& param =
			(index == 0 ? binding->type : binding->overloads[index - 1])
				->parameters[0];
		if (param->kind == TK_RVALUE_REFERENCE)
			cls->move_assign_index = (int)index;
		else
			cls->copy_assign_index = (int)index;
		BuildAssignSpecial(*cls, index, true);
		return;
	}
	Scope* declaring = ResolvePrefixScope(*composed.id);
	if (declaring->kind != SCOPE_CLASS &&
	    declaring->kind != SCOPE_NAMESPACE)
		throw OutsideBoundary("qualified declarator scope");
	const string& name = PartName(composed.id->parts.back());
	ScopeBinding* member = FindOwnBinding(*declaring, name);
	if (!member || member->kind != SB_VARIABLE)
		throw runtime_error(name + " is not a declared object");
	// 7.1.5p9: a constexpr object definition declares a const object.
	TypePtr defined_type = composed.type;
	if (specs.is_constexpr)
		defined_type = MakeCvQualifiedType(defined_type, true, false);
	member->type = MergeRedeclaredType(member->type, defined_type);
	// 9.4.2p2: the initializer of a static data member is in the scope
	// of its class - unqualified names there (member typedefs,
	// constants) resolve through the member scope.
	Scope* init_scope = declaring->kind == SCOPE_CLASS &&
		declarator.init ? declaring : current_;
	Scope* saved_scope = current_;
	current_ = init_scope;
	// PA18: the instantiated definition's constant value folds at
	// later uses within this translation unit (the checked outputs
	// pin the fold), and the object emits weak (14.7.1). PA23: the
	// fold is visible only to instantiated bodies - the definition's
	// point of instantiation sits after every parse-scope use
	// (14.6.4.1), so those keep loading the storage.
	if (instantiating_ && declarator.init && !member->has_value)
	{
		RecordConstantValue(*member, declarator.init.get());
		member->value_from_def = member->has_value;
		TypePtr member_bare = RemoveTopCv(member->type);
		if (!member->has_value && member->type->is_const &&
		    !member->type->is_volatile &&
		    member_bare->kind == TK_POINTER &&
		    member_bare->target->kind == TK_FUNCTION)
			member->fn_pointer_fold = true;
	}
	// The definition emits like a namespace-scope object owned by the
	// declaring scope. A qualified definition completes an earlier
	// (extern) declaration, so const-ness does not make it internal.
	SemNode* item = AppendItem(SN_VARIABLE);
	item->name = QualifiedScopePath(declaring) + name;
	item->type = member->type;
	item->entity_scope = declaring;
	item->entity_name = name;
	item->has_explicit_init = declarator.init != 0;
	item->is_static_decl = specs.is_static;
	item->is_thread_local_decl = specs.is_thread_local;
	item->is_extern_decl = declaring->kind == SCOPE_NAMESPACE;
	item->weak_def = instantiating_;
	// 9.4.2p3: a storage definition without its own initializer keeps
	// the evaluated in-class value; synthesizing default-construction
	// here would shadow it.
	bool inclass_object = !declarator.init &&
		engine_.FindObject(declaring, name) != 0;
	try
	{
		if (!inclass_object)
			AttachObjectLifetime(*item, *member, declarator.init.get(),
			                     specs);
	}
	catch (...)
	{
		current_ = saved_scope;
		throw;
	}
	current_ = saved_scope;
	AdoptInClassInitializer(*item, *member, !declarator.init,
	                        inclass_object);
	// PA20: an object-valued static member definition (9.4.2p3 storage
	// for an in-class constexpr initializer, or a defining braced
	// initializer) evaluates for the constant store and the image
	// emission of weak definitions.
	FinishConstexprObject(*item, *member, specs.is_constexpr);
}

// 9.4.2p3: a storage definition without its own initializer keeps the
// constant in-class initializer as the object's value: integral
// constants as a folded literal, other scalars (floats, pointers) as
// the analyzed in-class initializer so the definition renders the
// constant instead of a zero image. Object-valued members keep empty
// children (the evaluated image backs their emission).
void SemBinder::AdoptInClassInitializer(SemNode& item,
                                        const ScopeBinding& member,
                                        bool initializer_less,
                                        bool inclass_object)
{
	if (!initializer_less || !item.children.empty())
		return;
	if (member.has_value)
	{
		SemNodePtr constant = MakeSemNode(SN_LITERAL);
		constant->token = RenderConstValue(member.value);
		constant->type = RemoveTopCv(member.type);
		constant->category = VC_PRVALUE;
		constant->has_value = true;
		constant->value = member.value;
		item.children.push_back(std::move(constant));
		return;
	}
	if (!inclass_object)
		return;
	TypePtr bare = RemoveTopCv(member.type);
	if (bare->kind == TK_ARRAY || bare->kind == TK_CLASS ||
	    IsReferenceType(member.type))
		return;
	std::map<std::pair<const void*, string>,
	         SemNodePtr>::const_iterator held =
		member_image_inits_.find(std::pair<const void*, string>(
			member.owner, member.name));
	if (held == member_image_inits_.end() || !held->second)
		return;
	for (size_t i = 0; i < held->second->children.size(); i++)
		item.children.push_back(CloneSemNode(*held->second->children[i]));
}

// 6.8p1 disambiguation with name lookup: `begin(a);` is only a
// declaration when `begin` names a type; when it names a function the
// statement is the call expression instead. The parser commits to the
// declaration form, so the binder re-reads the shape here.
bool SemBinder::TryVexingCallRecovery(const AstDecl& decl)
{
	if (decl.kind != DK_SIMPLE || decl.declarators.size() != 1 ||
	    decl.specifiers.size() != 1 ||
	    decl.specifiers[0].kind != SPEC_TYPE_NAME)
		return false;
	const AstName& callee_name = decl.specifiers[0].name;
	if (TryResolveTypeFromName(callee_name))
		return false;
	const ScopeBinding* fn = 0;
	try
	{
		const NamedTypeInfo* member_class = 0;
		fn = ResolveValue(callee_name, member_class);
	}
	catch (const std::exception&)
	{
		return false;
	}
	if (!fn || fn->kind != SB_FUNCTION)
		return false;
	// Each init-declarator re-reads as one call argument: `(name)` as
	// an id-expression, `(&name)` as its address (`f(&a), (b);` is the
	// declaration reading of `f(&a, b);`).
	vector<AstExprPtr> arguments;
	for (size_t d = 0; d < decl.declarators.size(); d++)
	{
		const AstInitDeclarator& declarator = decl.declarators[d];
		if (declarator.init || !declarator.declarator ||
		    declarator.declarator->items.size() != 1 ||
		    declarator.declarator->items[0].kind != DI_NESTED)
			return false;
		const AstDeclarator* inner =
			declarator.declarator->items[0].nested.get();
		if (!inner)
			return false;
		bool address = false;
		bool call_form = false;
		size_t id_at = 0;
		if (inner->items.size() == 2 && inner->items[0].kind == DI_PTR &&
		    inner->items[0].token == OP_AMP)
		{
			address = true;
			id_at = 1;
		}
		else if (inner->items.size() == 2 &&
		         inner->items[0].kind == DI_ID &&
		         inner->items[1].kind == DI_PARAMS &&
		         inner->items[1].params &&
		         inner->items[1].params->parameters.empty() &&
		         !inner->items[1].params->variadic)
			// `f(g());`: the function declarator `g()` re-reads as
			// the argument call `g()` (the deque `clear()` shape,
			// `_M_erase_at_end(begin())` with the callee member
			// declared later in the class).
			call_form = true;
		else if (inner->items.size() != 1)
			return false;
		if (inner->items[id_at].kind != DI_ID ||
		    !inner->items[id_at].name.IsPlainIdentifier())
			return false;
		AstExprPtr argument(new AstExpr(EK_ID));
		AstNamePart arg_part;
		arg_part.kind = NP_IDENTIFIER;
		arg_part.identifier =
			inner->items[id_at].name.parts[0].identifier;
		argument->name.parts.push_back(std::move(arg_part));
		if (address)
		{
			AstExprPtr take(new AstExpr(EK_UNARY));
			take->op = OP_AMP;
			take->op_spelling = "&";
			take->operands.push_back(std::move(argument));
			argument = std::move(take);
		}
		else if (call_form)
		{
			AstExprPtr invoke(new AstExpr(EK_CALL));
			invoke->operands.push_back(std::move(argument));
			argument = std::move(invoke);
		}
		arguments.push_back(std::move(argument));
	}
	// Synthesize `callee(arguments...)` over owned AST nodes.
	AstExprPtr callee(new AstExpr(EK_ID));
	callee->name.global_scope = callee_name.global_scope;
	for (size_t i = 0; i < callee_name.parts.size(); i++)
	{
		AstNamePart part;
		part.kind = NP_IDENTIFIER;
		part.identifier = callee_name.parts[i].identifier;
		callee->name.parts.push_back(std::move(part));
	}
	AstExprPtr call(new AstExpr(EK_CALL));
	call->operands.push_back(std::move(callee));
	for (size_t d = 0; d < arguments.size(); d++)
		call->arguments.push_back(std::move(arguments[d]));
	SemNode* item = AppendItem(SN_EXPRESSION_STATEMENT);
	SemValue value = analyzer_.Analyze(*call);
	item->children.push_back(std::move(value.node));
	recovered_exprs_.push_back(std::move(call));
	return true;
}

bool SemBinder::InClassContextOrFriend(const NamedTypeInfo* cls)
{
	const ClassInfo* naming = unit_.classes.Find(cls);
	if (!naming)
		return false;
	vector<const ClassInfo*> contexts;
	if (method_.cls)
		contexts.push_back(method_.cls);
	if (method_.lexical_cls)
		contexts.push_back(method_.lexical_cls);
	for (const Scope* scope = current_; scope; scope = scope->parent)
		if (scope->kind == SCOPE_CLASS)
			if (const NamedTypeInfo* entity = model_.ScopeEntity(scope))
				if (const ClassInfo* context = unit_.classes.Find(entity))
					contexts.push_back(context);
	for (size_t i = 0; i < contexts.size(); i++)
	{
		if (contexts[i]->members == naming->members)
			return true;
		for (size_t j = 0; j < naming->friend_classes.size(); j++)
			if (FriendClassMatches(naming->friend_classes[j],
			                       contexts[i]->entity))
				return true;
	}
	if (!method_.fn_name.empty())
		for (size_t i = 0; i < naming->friend_functions.size(); i++)
			if (naming->friend_functions[i].first == method_.fn_owner &&
			    (naming->friend_functions[i].second == method_.fn_name ||
			     (!method_.fn_template_name.empty() &&
			      naming->friend_functions[i].second ==
			          method_.fn_template_name)))
				return true;
	return false;
}

TypePtr SemBinder::ResolveTypeName(const AstName& name)
{
	// 7.1.6.2p4: a decltype-specifier names the operand's type
	// directly (base clauses spell it as a base-type-specifier).
	if (name.parts.size() == 1 && name.parts[0].kind == NP_DECLTYPE &&
	    name.parts[0].decltype_expr)
		return ResolveDecltype(*name.parts[0].decltype_expr);
	// 14.6p3: a qualified dependent type whose qualifier names a
	// template parameter needs the `typename` disambiguator (base
	// clauses and other implicit type contexts do not, and pass a set
	// flag through the caller seam).
	if (name.parts.size() > 1 && !name.typename_keyword &&
	    !name.global_scope && !in_implicit_type_context_ &&
	    name.parts[0].kind == NP_IDENTIFIER && !name.parts[0].tilde)
	{
		const ScopeBinding* first = UnqualifiedLookup(
			current_, name.parts[0].identifier, SLF_ANY);
		if (first &&
		    (first->kind == SB_TYPE || first->kind == SB_TYPE_ALIAS) &&
		    first->home &&
		    first->home->kind == SCOPE_TEMPLATE_PARAMS)
			throw runtime_error("dependent qualified name " +
			                    name.parts[0].identifier +
			                    "::... needs typename");
	}
	const ScopeBinding* found = ResolveTerminal(name, SLF_ANY);
	if (!found)
	{
		if (TypePtr builtin = ResolveBuiltinTypeName(name))
			return builtin;
		throw runtime_error("undeclared type name " + TerminalName(name));
	}
	if (found->kind != SB_TYPE && found->kind != SB_TYPE_ALIAS)
		throw runtime_error(found->name + " does not name a type");
	if (found->home && found->home->kind == SCOPE_CLASS)
	{
		const NamedTypeInfo* entity = model_.ScopeEntity(found->home);
		if (!entity || !InClassContextOrFriend(entity))
			CheckMemberAccess(found->home, found->access, found->name);
	}
	return found->type;
}
