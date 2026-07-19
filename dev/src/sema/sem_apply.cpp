#include "sema/sem_expr.h"

#include <stdexcept>

#include "sema/scope_lookup.h"

using std::runtime_error;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA12 assignment boundary");
}

}  // namespace

// Conversion application, split from sem_expr.cpp: constructor and
// conversion-function calls, target-directed overload selection
// (13.4), copy-initialization, and the operand adjustment helpers the
// operator and call analyzers share.

// 12.3.1: the converting constructor builds a temporary of the
// destination class from the (standard-converted) source.
void SemExprAnalyzer::ApplyConstructorConversion(
	SemValue& value, const ImplicitConversion& conv)
{
	const ClassInfo* cls = host_.Classes().Find(conv.user_class);
	if (!cls)
		throw runtime_error("converting constructor class record "
		                    "missing");
	const TypePtr& param = cls->ctors[conv.user_ctor].type->parameters[0];
	ImplicitConversion inner =
		ClassifyConversion(MakeConversionSource(value), param);
	ApplyConversion(value, inner, param);
	vector<SemNodePtr> args;
	args.push_back(std::move(value.node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, conv.user_ctor, false, SemNodePtr(), std::move(args));
	action->category = VC_PRVALUE;
	if (host_.Classes().NeedsDestruction(*cls))
	{
		action->needs_dtor = true;
		action->children.push_back(host_.MakeTemporaryDtor(*cls));
	}
	TypePtr class_type = MakeNamedType(TK_CLASS, conv.user_class);
	action->type = class_type;
	value.node = std::move(action);
	value.type = class_type;
	value.category = VC_PRVALUE;
	value.null_pointer_literal = false;
}

// 13.4: the target type picked one overload; the id-expression node
// now shows the selected function's type, and a deduced specialization
// re-targets the node's identity.
void SemExprAnalyzer::ApplySelectedOverload(SemValue& value,
                                            const ImplicitConversion& conv,
                                            const TypePtr& dest)
{
	value.type = value.overloads[conv.selected_overload];
	if (!value.node)
	{
		// A member-access function set carries no expression node; the
		// selected (static) member synthesizes its id-expression here.
		value.node = MakeSemNode(SN_ID_EXPRESSION);
		value.node->category = value.category;
		if (value.member_fn)
		{
			value.node->name = value.member_fn->name;
			value.node->entity_scope = value.member_fn->owner;
			value.node->entity_name = value.member_fn->name;
		}
	}
	value.node->type = value.type;
	if ((size_t)conv.selected_overload < value.overload_specs.size())
		if (const FunctionSpecialization* spec =
		        value.overload_specs[conv.selected_overload])
		{
			value.node->entity_scope = spec->self.owner;
			value.node->entity_name = spec->self.name;
			value.node->fn_spec = spec;
			host_.OnSpecializationOdrUsed(spec);
			value.fn_owner = spec->self.owner;
			value.fn_name = spec->self.name;
		}
	value.function_set = false;
	value.overloads.clear();
	value.overload_specs.clear();
	if (value.fn_set_addressed)
	{
		// The set was spelled under & (5.3.1p6): the selected function
		// forms the pointer directly, with no decay step. A member
		// pointer destination forms the member pointer (5.3.1p4).
		TypePtr bare_dest = dest;
		if (bare_dest && IsReferenceType(bare_dest))
			bare_dest = bare_dest->target;
		if (bare_dest)
			bare_dest = RemoveTopCv(bare_dest);
		SemNodePtr address = MakeSemNode(SN_UNARY_EXPRESSION);
		if (bare_dest && bare_dest->kind == TK_MEMBER_POINTER)
		{
			address->type = MakeMemberPointerType(
				bare_dest->named, value.type, false, false);
			// The member-function entry keys on the this-adjusted
			// member identity.
			value.node->type = ThisAdjustedType(
				value.node->entity_scope
					? host_.Model().ScopeEntity(
					      value.node->entity_scope)
					: bare_dest->named,
				value.type);
		}
		else
			address->type = MakePointerType(value.type, false, false);
		address->category = VC_PRVALUE;
		address->has_op = true;
		address->op = OP_AMP;
		address->op_spelling = "&";
		address->children.push_back(std::move(value.node));
		value.type = address->type;
		value.node = std::move(address);
		value.category = VC_PRVALUE;
		value.fn_set_addressed = false;
	}
}

// PA24 8.5.4: builds the selected parameter's initialization from an
// analyzed braced-init-list argument: a temporary array for (reference
// -to-)array destinations, a list-constructed temporary for class
// destinations, the single element for scalars.
void SemExprAnalyzer::ApplyListInitConversion(SemValue& value,
                                              const ImplicitConversion& conv,
                                              const TypePtr& dest)
{
	vector<SemValue> items;
	items.swap(value.list_values);
	value.braced_list = false;
	TypePtr bare = IsReferenceType(dest) ? dest->target : RemoveTopCv(dest);
	// PA25 8.5.4p4: the initializer_list value is a typed braced node;
	// the lowering materializes the backing array and the
	// {begin, size} record.
	TypePtr list_element;
	if (conv.init_list_dest && IsStdInitializerList(bare, &list_element))
	{
		TypePtr element = RemoveTopCv(list_element);
		host_.RequireCompleteType(RemoveTopCv(bare)->named);
		SemNodePtr list = MakeSemNode(SN_BRACED_INIT_LIST);
		list->type = MakeNamedType(TK_CLASS, RemoveTopCv(bare)->named);
		list->category = VC_PRVALUE;
		for (size_t i = 0; i < items.size(); i++)
		{
			CopyInitialize(items[i], element, "initializer-list "
			               "element");
			list->children.push_back(std::move(items[i].node));
		}
		value.type = list->type;
		value.category = VC_PRVALUE;
		value.node = std::move(list);
		return;
	}
	if (bare->kind == TK_ARRAY)
	{
		TypePtr element = bare->target;
		SemNodePtr list = MakeSemNode(SN_BRACED_INIT_LIST);
		list->type = RemoveTopCv(bare);
		list->category = VC_PRVALUE;
		for (size_t i = 0; i < items.size(); i++)
		{
			CopyInitialize(items[i], RemoveTopCv(element),
			               "array element");
			list->children.push_back(std::move(items[i].node));
		}
		value.type = RemoveTopCv(bare);
		value.category = VC_PRVALUE;
		value.node = std::move(list);
		return;
	}
	// PA34 8.5.1: an aggregate destination takes the field-wise
	// temporary (designated elements realign inside).
	if (conv.aggregate_list && conv.user_class)
	{
		const ClassInfo* cls = host_.Classes().Find(conv.user_class);
		if (!cls)
			throw runtime_error("aggregate class record missing");
		SemNodePtr action =
			host_.MakeAggregateTemporary(*cls, std::move(items));
		TypePtr class_type = MakeNamedType(TK_CLASS, conv.user_class);
		action->type = class_type;
		action->category = VC_PRVALUE;
		value.node = std::move(action);
		value.type = class_type;
		value.category = VC_PRVALUE;
		return;
	}
	if (conv.user_ctor >= 0 && conv.user_class)
	{
		const ClassInfo* cls = host_.Classes().Find(conv.user_class);
		if (!cls)
			throw runtime_error("list constructor class record missing");
		const TypePtr& fn = cls->ctors[conv.user_ctor].type;
		vector<SemNodePtr> args;
		for (size_t i = 0; i < items.size(); i++)
		{
			ImplicitConversion inner = ClassifyConversion(
				MakeConversionSource(items[i]), fn->parameters[i]);
			ApplyConversion(items[i], inner, fn->parameters[i]);
			args.push_back(std::move(items[i].node));
		}
		SemNodePtr action = host_.MakeConstructorCall(
			*cls, conv.user_ctor, false, SemNodePtr(), std::move(args));
		action->category = VC_PRVALUE;
		if (host_.Classes().NeedsDestruction(*cls))
		{
			action->needs_dtor = true;
			action->children.push_back(host_.MakeTemporaryDtor(*cls));
		}
		TypePtr class_type = MakeNamedType(TK_CLASS, conv.user_class);
		action->type = class_type;
		value.node = std::move(action);
		value.type = class_type;
		value.category = VC_PRVALUE;
		return;
	}
	// 8.5.4p3: the single element initializes a scalar; an empty list
	// zero-initializes.
	if (items.size() == 1)
	{
		value = std::move(items[0]);
		CopyInitialize(value, RemoveTopCv(bare), "list element");
		return;
	}
	if (!items.empty())
		throw runtime_error("too many list-initializer elements");
	value.type = RemoveTopCv(bare);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = "0";
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	if (value.type->kind == TK_POINTER || IsNullPtrType(value.type))
		value.node->null_pointer = true;
	else
	{
		value.node->has_value = true;
		value.node->value = ConstValue(
			value.type->kind == TK_ENUM
				? value.type->named->enum_underlying
				: value.type->fundamental,
			0);
	}
}

void SemExprAnalyzer::ApplyConversion(SemValue& value,
                                      const ImplicitConversion& conv,
                                      const TypePtr& dest)
{
	if (value.braced_list)
	{
		ApplyListInitConversion(value, conv, dest);
		return;
	}
	if (conv.closure_to_pointer)
	{
		// PA24 5.1.2p6: the captureless closure's value becomes its
		// function (the lowering spells the address and decay).
		const Scope* owner = 0;
		string name;
		TypePtr fn_type;
		TypePtr closure = value.type;
		if (IsReferenceType(closure))
			closure = closure->target;
		if (!host_.CapturelessClosureFunction(
		        RemoveTopCv(closure)->named, owner, name, fn_type))
			throw runtime_error("closure function record missing");
		value = SemValue();
		value.type = fn_type;
		value.category = VC_LVALUE;
		value.node = MakeSemNode(SN_ID_EXPRESSION);
		value.node->name = name;
		value.node->type = fn_type;
		value.node->category = VC_LVALUE;
		value.node->entity_scope = owner;
		value.node->entity_name = name;
		return;
	}
	if (conv.user_ctor >= 0 && conv.user_class)
	{
		ApplyConstructorConversion(value, conv);
		return;
	}
	if (conv.conv_index >= 0 && conv.conv_class)
	{
		// 12.3.2: the conversion function call produces the converted
		// value; any remaining standard conversion happens in context.
		const ClassInfo* cls = host_.Classes().Find(conv.conv_class);
		if (!cls)
			throw runtime_error("conversion function class record "
			                    "missing");
		const ClassConversion& fn = cls->conversions[conv.conv_index];
		host_.CheckMemberAccess(cls->members, fn.access, fn.name);
		int hops = 0;
		unsigned long long base_offset = 0;
		int vbase_index = -1;
		// The conversion set was collected along the base DAG, so the
		// owning class resolves; a non-unique non-virtual path means the
		// inherited conversion's subobject is ambiguous (10.2). A shared
		// virtual-base owner is one subobject behind a virtual edge.
		const NamedTypeInfo* source = RemoveTopCv(value.type)->named;
		EBasePath path = BaseSubobjectPath(source, conv.conv_class, hops,
		                                   base_offset);
		if (path != BP_UNIQUE)
		{
			size_t carrier = 0;
			unsigned long long remainder = 0;
			if (path == BP_NONE && source && source->class_record &&
			    VirtualBasePath(*source->class_record, conv.conv_class,
			                    carrier, remainder))
			{
				hops = 1;
				base_offset = remainder;
				vbase_index = (int)carrier;
			}
			else
				throw runtime_error("ambiguous base class subobject");
		}
		if (hops > 0)
		{
			SemNodePtr adjusted = MakeSemNode(SN_MEMBER_EXPRESSION);
			adjusted->type = MakeNamedType(TK_CLASS, conv.conv_class);
			adjusted->category = VC_LVALUE;
			adjusted->base_hops = hops;
			adjusted->base_offset = base_offset;
			adjusted->vbase_index = vbase_index;
			adjusted->children.push_back(std::move(value.node));
			value.node = std::move(adjusted);
		}
		SemValue out = CallResult(fn.type);
		SemNodePtr callee = MakeSemNode(SN_CALLEE);
		if (fn.spec)
		{
			// A deduced conversion-template specialization routes like
			// a namespace-scope specialization (its own entry keyed on
			// the argument alias scope), with the object address as
			// the leading argument.
			callee->name = CanonicalQualifiedName(fn.spec->self.owner,
			                                      fn.spec->name);
			callee->type = ThisAdjustedType(conv.conv_class, fn.type);
			callee->entity_scope = fn.spec->self.owner;
			callee->entity_name = fn.spec->name;
			callee->fn_spec = fn.spec;
			host_.OnSpecializationOdrUsed(fn.spec);
		}
		else
		{
			callee->name = CanonicalQualifiedName(cls->members, fn.name);
			callee->type = ThisAdjustedType(conv.conv_class, fn.type);
			callee->entity_scope = cls->members;
			callee->entity_name = fn.name;
			callee->is_method = true;
		}
		if (const ScopeBinding* binding = fn.spec
		        ? 0 : FindOwnBinding(*cls->members, fn.name))
		{
			size_t index = 0;
			for (size_t i = 0; i < binding->overloads.size(); i++)
				if (TypeEquals(binding->overloads[i], fn.type))
					index = i + 1;
			if (index < binding->fn_unwind_no.size() &&
			    binding->fn_unwind_no[index])
				callee->unwind_no = true;
			if (index < binding->fn_noexcept_decl.size() &&
			    binding->fn_noexcept_decl[index])
				callee->noexcept_decl = true;
		}
		out.node->children.push_back(std::move(callee));
		out.node->children.push_back(
			AddressOfObject(std::move(value.node)));
		out.node->user_conversion = true;
		bool null_literal = value.null_pointer_literal;
		(void)null_literal;
		value = std::move(out);
		return;
	}
	if (conv.selected_overload >= 0)
		ApplySelectedOverload(value, conv, dest);
	if (conv.null_to_pointer)
	{
		// The dump retypes a converted null pointer literal in place.
		TypePtr target = IsReferenceType(dest)
			? RemoveTopCv(dest->target) : RemoveTopCv(dest);
		value.type = target;
		value.node->type = target;
		value.node->null_pointer = true;
		value.null_pointer_literal = false;
	}
	if (!IsReferenceType(dest))
	{
		// PA16: a by-value class destination copy/move-initializes from
		// a class source unless the source already constructs the
		// destination object in place.
		TypePtr bare = RemoveTopCv(dest);
		if (bare->kind == TK_CLASS && value.type && !value.function_set &&
		    RemoveTopCv(value.type)->kind == TK_CLASS)
		{
			// A same-class prvalue (temporary, call result) constructs
			// the destination in place (12.8p31). A conditional does
			// so only for a trivially copyable class (its arms lower
			// as raw object copies); otherwise it materializes its own
			// temporary and the destination copy/move-constructs.
			bool in_place = value.node &&
				value.category == VC_PRVALUE &&
				RemoveTopCv(value.type)->named == bare->named;
			if (in_place &&
			    value.node->kind == SN_CONDITIONAL_EXPRESSION)
			{
				const ClassInfo* cls =
					host_.Classes().Find(bare->named);
				in_place = cls && ClassHasTrivialCopyCtor(*cls);
			}
			if (!in_place)
				WrapClassValueInit(value, bare);
		}
	}
}

void SemExprAnalyzer::WrapClassValueInit(SemValue& value, const TypePtr& bare)
{
	host_.RequireCompleteType(bare->named);
	const ClassInfo* cls = host_.Classes().Find(bare->named);
	if (!cls || !bare->named->complete)
		throw runtime_error("copy of an incomplete class object");
	vector<SemValue> args;
	args.push_back(std::move(value));
	int index = host_.ResolveClassCtorHost(*cls, args, true,
	                                       "initialization");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < args.size(); i++)
		arg_nodes.push_back(std::move(args[i].node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, index, false, SemNodePtr(), std::move(arg_nodes));
	action->synth_copy = true;
	action->type = RemoveTopCv(bare);
	action->category = VC_PRVALUE;
	if (host_.Classes().NeedsDestruction(*cls))
	{
		action->needs_dtor = true;
		action->children.push_back(host_.MakeTemporaryDtor(*cls));
	}
	value = SemValue();
	value.type = action->type;
	value.category = VC_PRVALUE;
	value.node = std::move(action);
}

void SemExprAnalyzer::CopyInitialize(SemValue& value, const TypePtr& dest,
                                     const char* what)
{
	AddTargetDeducedOverloads(value, dest);
	ImplicitConversion conv =
		ClassifyConversion(MakeConversionSource(value), dest);
	if (!conv.viable)
		throw runtime_error(string("no conversion for ") + what);
	ApplyConversion(value, conv, dest);
}

void SemExprAnalyzer::RequireContextualBool(SemValue& value,
                                            const char* what)
{
	// 4p4: contextual bool conversion; explicit conversion functions
	// participate (direct-initialization semantics).
	ImplicitConversion conv = ClassifyConversionEx(
		MakeConversionSource(value), MakeFundamentalType(FT_BOOL), true);
	if (!conv.viable)
		throw runtime_error(string(what) + " is not contextually bool");
	if (conv.conv_index >= 0 && value.node)
		ApplyConversion(value, conv, MakeFundamentalType(FT_BOOL));
}

// A class operand of a built-in operator form converts through its
// single viable non-explicit conversion function (the 13.6 subset).
bool SemExprAnalyzer::ConvertClassOperand(SemValue& value)
{
	TypePtr bare = RemoveTopCv(value.type);
	if (bare->kind != TK_CLASS || !bare->named->class_record)
		return false;
	bool source_const = false;
	bool source_volatile = false;
	TopCv(value.type, source_const, source_volatile);
	const ClassConversion* found = 0;
	int found_score = 3;
	vector<const ClassInfo*> subtree;
	CollectClassAndBases(bare->named->class_record, subtree);
	for (size_t c = 0; c < subtree.size(); c++)
	{
		const ClassInfo* link = subtree[c];
		for (size_t i = 0; i < link->conversions.size(); i++)
		{
			const ClassConversion& conv = link->conversions[i];
			if (conv.is_explicit)
				continue;
			// The implicit object binding selects among cv-qualified
			// overloads: a const source requires a const function.
			if (source_const && !conv.type->is_const)
				continue;
			int score = conv.type->is_const == source_const ? 0 : 1;
			if (found && score == found_score &&
			    !TypeEquals(found->result, conv.result))
				return false;  // no unique built-in operand form
			if (!found || score < found_score)
			{
				found = &conv;
				found_score = score;
			}
		}
	}
	if (!found)
		return false;
	TypePtr dest = IsReferenceType(found->result)
		? found->result->target : RemoveTopCv(found->result);
	ImplicitConversion conv = ClassifyConversionEx(
		MakeConversionSource(value), dest, false);
	if (!conv.viable || conv.conv_index < 0)
		return false;
	ApplyConversion(value, conv, dest);
	return true;
}

void SemExprAnalyzer::RequireModifiableLvalue(const SemValue& value,
                                              const char* what)
{
	if (value.category != VC_LVALUE)
		throw runtime_error(string(what) + " requires an lvalue");
	bool is_const = false;
	bool is_volatile = false;
	TopCv(value.type, is_const, is_volatile);
	if (is_const || value.type->kind == TK_ARRAY ||
	    value.type->kind == TK_FUNCTION || value.function_set)
		throw runtime_error(string(what) + " requires a modifiable lvalue");
}

// 8.5.1 array list-initialization (split from sem_expr.cpp):
// braced arrays complete their bounds and convert element-wise.
SemNodePtr SemExprAnalyzer::AnalyzeBracedInit(
	const vector<AstExprPtr>& items, TypePtr& dest)
{
	if (dest->kind != TK_ARRAY)
		throw OutsideBoundary("braced initialization form");
	// PA20: a multi-dimensional array initializes element-wise from
	// fully braced sub-lists (8.5.1p11 without brace elision).
	if (RemoveTopCv(dest->target)->kind == TK_ARRAY)
	{
		if (!dest->bound_known)
			dest = MakeArrayType(dest->target, true, items.size());
		if (items.size() > dest->bound)
			throw runtime_error("too many braced initializers");
		SemNodePtr node = MakeSemNode(SN_BRACED_INIT_LIST);
		node->type = dest;
		node->category = VC_LVALUE;
		for (size_t i = 0; i < items.size(); i++)
		{
			const AstExpr& element = *items[i];
			if (element.kind != EK_BRACED)
				throw OutsideBoundary("array-of-array element form");
			TypePtr element_type = dest->target;
			node->children.push_back(
				AnalyzeBracedInit(element.arguments, element_type));
		}
		return node;
	}
	// Pack expansions among the elements resolve before the bound
	// completes (8.5.1p4 over the expanded list).
	vector<SemValue> elements;
	AnalyzeArgumentList(items, elements);
	if (dest->bound_known && elements.size() > dest->bound)
		throw runtime_error("too many braced initializers");
	if (!dest->bound_known)
		// 8.5.1p4: the array bound completes from the initializer list.
		dest = MakeArrayType(dest->target, true, elements.size());
	SemNodePtr node = MakeSemNode(SN_BRACED_INIT_LIST);
	node->type = dest;
	node->category = VC_LVALUE;
	for (size_t i = 0; i < elements.size(); i++)
	{
		CopyInitialize(elements[i], dest->target, "array element");
		node->children.push_back(std::move(elements[i].node));
	}
	return node;
}
