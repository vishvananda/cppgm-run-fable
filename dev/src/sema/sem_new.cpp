#include "sema/sem_expr.h"

#include <stdexcept>

#include "sema/const_expr.h"
#include "sema/scope_lookup.h"

using std::runtime_error;

// PA16 allocation expressions (5.3.4/5.3.5): scalar and array new over
// the supported object subset, placement and nothrow forms, and
// delete / delete[] with element destruction. Allocation lowers as
// ordinary construction/destruction actions over explicit storage.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA16 assignment boundary");
}

// The first array bound of a new-type-id declarator, or npos.
size_t FindNewArrayBound(const AstTypeId& type_id)
{
	if (!type_id.declarator)
		return (size_t)-1;
	for (size_t i = 0; i < type_id.declarator->items.size(); i++)
		if (type_id.declarator->items[i].kind == DI_ARRAY)
			return i;
	return (size_t)-1;
}

}  // namespace

// The global allocation or deallocation function: overload selection
// over (size/pointer, placement...) with the conversion machinery.
SemValue SemExprAnalyzer::MakeAllocationCall(const char* name,
                                             vector<SemValue> args,
                                             const TypePtr& result_type,
                                             bool& unwind_no)
{
	const ScopeBinding* binding =
		FindOwnBinding(*host_.Model().global(), name);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error(string("no declaration of ") + name);
	vector<TypePtr> candidates;
	candidates.push_back(binding->type);
	for (size_t i = 0; i < binding->overloads.size(); i++)
		candidates.push_back(binding->overloads[i]);
	vector<ConversionSource> sources;
	for (size_t i = 0; i < args.size(); i++)
		sources.push_back(MakeConversionSource(args[i]));
	vector<ImplicitConversion> conversions;
	size_t winner;
	try
	{
		winner = SelectBestOverload(candidates, sources, conversions);
	}
	catch (const NoViableOverloadError&)
	{
		throw NoViableOverloadError(
			string("no matching function for call to ") + name);
	}
	const TypePtr& fn = candidates[winner];
	for (size_t i = 0; i < args.size(); i++)
		ApplyConversion(args[i], conversions[i], fn->parameters[i]);
	host_.ResolveNoexceptFacts(*binding, winner);
	unwind_no = winner < binding->fn_unwind_no.size() &&
		binding->fn_unwind_no[winner];
	// 18.6.1.3: the reserved placement form returns its pointer
	// argument and cannot fail; no null check guards construction.
	if (fn->parameters.size() == 2 &&
	    fn->parameters[1]->kind == TK_POINTER &&
	    IsVoidType(RemoveTopCv(fn->parameters[1]->target)))
		unwind_no = false;
	SemValue value;
	value.type = result_type;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_CALL_EXPRESSION);
	value.node->type = result_type;
	value.node->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding->owner, binding->name);
	callee->type = fn;
	callee->entity_scope = binding->owner;
	callee->entity_name = binding->name;
	callee->unwind_no = unwind_no;
	value.node->children.push_back(std::move(callee));
	for (size_t i = 0; i < args.size(); i++)
		value.node->children.push_back(std::move(args[i].node));
	return value;
}

SemValue SemExprAnalyzer::MakeSizeLiteral(unsigned long long size)
{
	SemValue value;
	value.node = MakeSemNode(SN_LITERAL);
	value.node->token = std::to_string(size);
	value.node->type = MakeFundamentalType(FT_INT);
	value.node->category = VC_PRVALUE;
	value.node->has_value = true;
	value.node->value = ConstValue(FT_INT, size);
	value.type = value.node->type;
	return value;
}

// 5.3.4 array form: the count splits off the new-type-id; class
// elements carry an 8-byte count header and a per-element constructor.
SemValue SemExprAnalyzer::AnalyzeNewArray(const AstExpr& expr,
                                          size_t bound_item)
{
	if (expr.has_placement)
		throw OutsideBoundary("placement array new");
	AstDeclarator& declarator =
		*const_cast<AstTypeId&>(*expr.type).declarator;
	AstExprPtr bound = std::move(declarator.items[bound_item].array_bound);
	declarator.items.erase(declarator.items.begin() + bound_item);
	TypePtr element;
	try
	{
		element = RemoveTopCv(host_.ResolveCastTypeId(*expr.type));
	}
	catch (...)
	{
		declarator.items.insert(declarator.items.begin() + bound_item,
		                        AstDeclaratorItem());
		declarator.items[bound_item].kind = DI_ARRAY;
		declarator.items[bound_item].array_bound = std::move(bound);
		throw;
	}
	declarator.items.insert(declarator.items.begin() + bound_item,
	                        AstDeclaratorItem());
	declarator.items[bound_item].kind = DI_ARRAY;
	declarator.items[bound_item].array_bound = std::move(bound);
	const AstExpr& count_expr =
		*declarator.items[bound_item].array_bound;

	if (RemoveTopCv(element)->kind == TK_CLASS)
		host_.RequireCompleteType(RemoveTopCv(element)->named);
	const ClassInfo* cls = RemoveTopCv(element)->kind == TK_CLASS
		? host_.Classes().Find(RemoveTopCv(element)->named) : 0;
	if (RemoveTopCv(element)->kind == TK_CLASS &&
	    (!cls || !RemoveTopCv(element)->named->complete))
		throw runtime_error("array new of an incomplete class");
	SemValue count;
	ConstValue const_count;
	if (host_.TryEvaluateConstant(count_expr, const_count))
	{
		count = MakeSizeLiteral(const_count.bits);
		count.node->value = ConvertConstValue(const_count, FT_INT);
		count.node->token = RenderConstValue(count.node->value);
	}
	else
		count = Analyze(count_expr);

	const char* alloc_name = "operator new[]";
	const ScopeBinding* binding =
		FindOwnBinding(*host_.Model().global(), alloc_name);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error("no declaration of operator new[]");
	TypePtr pointer = MakePointerType(element, false, false);
	SemValue value;
	value.type = pointer;
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(SN_NEW_ARRAY);
	value.node->type = pointer;
	value.node->category = VC_PRVALUE;
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding->owner, binding->name);
	callee->type = binding->type;
	callee->entity_scope = binding->owner;
	callee->entity_name = binding->name;
	value.node->children.push_back(std::move(callee));
	value.node->children.push_back(std::move(count.node));
	value.node->has_value = true;
	value.node->value =
		ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(element));
	if (cls)
	{
		value.node->member_offset = 8;
		// Default-construction of every element when the class needs
		// constructor code; the action carries no address.
		vector<SemValue> no_args;
		int index = host_.ResolveClassCtorHost(*cls, no_args, false,
		                                       "array new");
		if (cls->has_user_ctor || host_.Classes().NeedsConstruction(*cls))
		{
			SemNodePtr action = host_.MakeConstructorCall(
				*cls, index, false, SemNodePtr(), vector<SemNodePtr>());
			if (!action->trivial_init)
				value.node->children.push_back(std::move(action));
		}
	}
	else if (expr.new_init)
	{
		// `()` value-initialization zero-fills the elements.
		if (expr.new_init->kind != INIT_PAREN ||
		    !expr.new_init->args.empty())
			throw OutsideBoundary("array new initializer form");
		value.node->trivial_init = true;
	}
	return value;
}

// 5.3.4: placement and non-placement scalar new over the supported
// object subset.
SemValue SemExprAnalyzer::AnalyzeNew(const AstExpr& expr)
{
	if (!expr.type)
		throw OutsideBoundary("new-expression form");
	size_t bound_item = FindNewArrayBound(*expr.type);
	if (bound_item != (size_t)-1)
		return AnalyzeNewArray(expr, bound_item);
	TypePtr allocated = RemoveTopCv(host_.ResolveCastTypeId(*expr.type));
	if (allocated->kind == TK_CLASS)
		host_.RequireCompleteType(allocated->named);
	const ClassInfo* cls = allocated->kind == TK_CLASS
		? host_.Classes().Find(allocated->named) : 0;
	if (allocated->kind == TK_CLASS &&
	    (!cls || !allocated->named->complete))
		throw runtime_error("new of an incomplete class");
	// The allocation call: operator new(sizeof(T), placement...).
	vector<SemValue> alloc_args;
	alloc_args.push_back(MakeSizeLiteral(TypeSize(allocated)));
	for (size_t i = 0; i < expr.arguments.size(); i++)
		alloc_args.push_back(Analyze(*expr.arguments[i]));
	TypePtr pointer = MakePointerType(allocated, false, false);
	bool unwind_no = false;
	SemValue alloc = MakeAllocationCall("operator new",
	                                    std::move(alloc_args), pointer,
	                                    unwind_no);
	if (!cls)
	{
		// A non-class object: the converted initializer value stores
		// into the allocated storage.
		SemValue value;
		value.type = pointer;
		value.category = VC_PRVALUE;
		value.node = MakeSemNode(SN_NEW_INIT);
		value.node->type = pointer;
		value.node->category = VC_PRVALUE;
		value.node->null_pointer = unwind_no;
		value.node->children.push_back(std::move(alloc.node));
		if (expr.new_init)
		{
			const AstInitializer& init = *expr.new_init;
			vector<SemValue> given_args;
			if (init.kind == INIT_PAREN)
				AnalyzeArgumentList(init.args, given_args);
			else if (init.kind == INIT_BRACED)
				AnalyzeArgumentList(init.expr->arguments, given_args);
			else
				throw OutsideBoundary("new initializer form");
			if (given_args.size() > 1)
				throw runtime_error("too many initializers in new");
			bool has_arg = !given_args.empty();
			SemValue stored;
			if (has_arg)
			{
				stored = std::move(given_args[0]);
				CopyInitialize(stored, allocated, "new initializer");
			}
			else
				stored = MakeSizeLiteral(0);  // value-initialization
			if (!has_arg)
			{
				stored.node->type = allocated;
				stored.node->value =
					ConstValue(allocated->kind == TK_ENUM
					           ? allocated->named->enum_underlying
					           : allocated->fundamental, 0);
				stored.node->null_pointer =
					allocated->kind == TK_POINTER;
				stored.type = allocated;
			}
			value.node->children.push_back(std::move(stored.node));
		}
		return value;
	}
	// The constructor over the allocation's result.
	vector<SemValue> ctor_args;
	if (expr.new_init)
	{
		if (expr.new_init->kind != INIT_PAREN)
			throw OutsideBoundary("new-initializer form");
		AnalyzeArgumentList(expr.new_init->args, ctor_args);
	}
	int ctor_index = host_.ResolveClassCtorHost(*cls, ctor_args, false,
	                                            "new-expression");
	vector<SemNodePtr> arg_nodes;
	for (size_t i = 0; i < ctor_args.size(); i++)
		arg_nodes.push_back(std::move(ctor_args[i].node));
	SemNodePtr action = host_.MakeConstructorCall(
		*cls, ctor_index, false, std::move(alloc.node),
		std::move(arg_nodes));
	// A new-initializer always calls the selected constructor; a
	// trivially-transferable implicit copy/move still needs its
	// synthesized body behind the call.
	if (ctor_index >= 0 && action->trivial_copy)
		host_.EnsureSpecialCtorHost(*cls, ctor_index);
	action->type = pointer;
	action->category = VC_PRVALUE;
	action->null_pointer = unwind_no;
	SemValue value;
	value.type = pointer;
	value.category = VC_PRVALUE;
	value.node = std::move(action);
	return value;
}

// 5.3.5: delete / delete[] over the supported object subset.
SemValue SemExprAnalyzer::AnalyzeDelete(const AstExpr& expr)
{
	SemValue operand = Analyze(*expr.operands[0]);
	if (operand.type->kind != TK_POINTER)
		throw runtime_error("delete requires a pointer operand");
	TypePtr element = RemoveTopCv(operand.type->target);
	bool array_form = expr.array_delete;
	const char* fn_name = array_form ? "operator delete[]"
	                                 : "operator delete";
	const ScopeBinding* binding =
		FindOwnBinding(*host_.Model().global(), fn_name);
	if (!binding || binding->kind != SB_FUNCTION)
		throw runtime_error(string("no declaration of ") + fn_name);
	SemValue value;
	value.type = MakeFundamentalType(FT_VOID);
	value.category = VC_PRVALUE;
	value.node = MakeSemNode(array_form ? SN_DELETE_ARRAY
	                                    : SN_DELETE_EXPRESSION);
	value.node->type = value.type;
	value.node->category = VC_PRVALUE;
	value.node->children.push_back(std::move(operand.node));
	SemNodePtr callee = MakeSemNode(SN_CALLEE);
	callee->name = CanonicalQualifiedName(binding->owner, binding->name);
	callee->type = binding->type;
	callee->entity_scope = binding->owner;
	callee->entity_name = binding->name;
	callee->unwind_no = true;
	value.node->children.push_back(std::move(callee));
	value.node->has_value = true;
	value.node->value =
		ConstValue(FT_UNSIGNED_LONG_INT, TypeSize(element));
	if (element->kind == TK_CLASS)
	{
		const ClassInfo* cls = host_.Classes().Find(element->named);
		if (!cls || !element->named->complete)
			throw runtime_error("delete of an incomplete class");
		if (array_form)
			value.node->member_offset = 8;
		// PA17 5.3.5p3: scalar delete of a class with a virtual
		// destructor dispatches through the deleting-destructor vtable
		// slot (which also frees the storage) even when the static
		// class's own destruction chain is effect-free - the dynamic
		// type may add destruction work.
		bool virtual_delete = !array_form && cls->dtor_virtual &&
			cls->dtor_slot >= 0;
		if (host_.Classes().NeedsDestruction(*cls) || virtual_delete)
		{
			// 5.3.5p6: the deleted object's destructor runs even when
			// the chain is effect-free.
			SemNodePtr dtor = host_.MakeTemporaryDtor(*cls);
			if (virtual_delete)
				dtor->children[0]->children[0]->vtable_slot =
					cls->dtor_slot + 1;
			value.node->children.push_back(std::move(dtor));
		}
	}
	return value;
}
