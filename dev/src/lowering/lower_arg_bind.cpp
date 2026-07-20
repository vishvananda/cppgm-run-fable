#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"
#include "sema/sem_convert.h"

// Call-argument binding: parameter-directed argument lowering
// (5.2.2), reference binding of argument expressions (8.5.3), and
// class-argument materialization at the ABI boundary. The call
// orchestration itself lives in lower_expr.cpp.

namespace {

TypePtr StripRef(const TypePtr& type)
{
	return IsReferenceType(type) ? type->target : type;
}

// The computation type of a node: declared type with references and
// top-level cv stripped.
TypePtr NodeType(const SemNode& node)
{
	return RemoveTopCv(StripRef(node.type));
}

}  // namespace

string FunctionLowerer::LowerCallArgument(const SemNode& node,
                                          const TypePtr& param)
{
	if (!param)
	{
		// 5.2.2p7 default argument promotions for trailing variadic
		// arguments.
		TypePtr source = NodeType(node);
		TypePtr promoted = source;
		if (source->kind == TK_FUNDAMENTAL &&
		    source->fundamental == FT_FLOAT)
			promoted = MakeFundamentalType(FT_DOUBLE);
		else if (IsIntegralType(source) || IsUnscopedEnum(source))
			promoted = PromoteForArithmetic(source);
		else if (source->kind == TK_ARRAY ||
		         source->kind == TK_FUNCTION)
			return LowerPointerOperand(node);
		else if (RemoveTopCv(source)->kind == TK_CLASS)
			// 5.2.2p7: a class through the ellipsis is conditionally
			// supported; the reference passes the object's address.
			return MaterializeClassArg(node, RemoveTopCv(source));
		return LowerValueAs(node, promoted, LCC_INIT);
	}
	if (IsReferenceType(param))
		return LowerReferenceArgument(node, param->target);
	if (RemoveTopCv(param)->kind == TK_CLASS)
	{
		TypePtr bare = RemoveTopCv(param);
		if (!LowerClassDirect(bare, program_.SeparateCompilation()))
			// The indirect boundary: the caller materializes the
			// argument object and passes its address.
			return MaterializeClassArg(node, bare);
		// The direct obj boundary: a fresh object slot passes by name.
		string slot = AddMatSlot("argobj", LowerSlotType(bare));
		string copy_address = NewTemp();
		Emit(copy_address + " = addr $" + slot);
		if (node.kind == SN_CONSTRUCTOR_ACTION && node.trivial_init)
			;  // default-constructed argument object: storage only
		else if (node.kind == SN_ID_EXPRESSION ||
		         node.kind == SN_MEMBER_EXPRESSION)
		{
			// The PA15 empty-class direct binding kept the slot
			// untouched; non-empty trivial sources copy their bytes.
			string source_address = LowerAddressExpr(node);
			if (bare->named->class_record &&
			    !bare->named->class_record->is_empty)
				Emit("copyobj " + LowerObjSpan(bare) + " " +
				     source_address + ", " + copy_address);
		}
		else
			LowerClassInit(node, copy_address);
		return "$" + slot;
	}
	return LowerValueAs(node, RemoveTopCv(param), LCC_INIT);
}

string FunctionLowerer::MaterializeClassArg(const SemNode& node,
                                            const TypePtr& bare)
{
	if (node.kind == SN_CONSTRUCTOR_ACTION)
		// The callee destroys its by-value parameter object;
		// no caller-side cleanup registers.
		return MaterializeTemporary(node, "arg", false);
	string slot = AddMatSlot("arg", LowerSlotType(bare));
	string address = NewTemp();
	Emit(address + " = addr $" + slot);
	LowerClassInit(node, address);
	return address;
}

string FunctionLowerer::LowerReferenceArgument(const SemNode& node,
                                               const TypePtr& referee)
{
	TypePtr bare = RemoveTopCv(referee);
	if (node.kind == SN_BRACED_INIT_LIST && bare->kind == TK_ARRAY)
	{
		// PA24 8.5.4: a braced argument bound to a reference-to-array
		// parameter materializes its temporary array.
		string slot = AddMatSlot("argarr", LowerSlotType(bare));
		return LowerLocalArrayInit(node, slot, bare);
	}
	if (node.kind == SN_CLOSURE_INIT && bare->kind == TK_CLASS)
	{
		// PA24: a closure temporary bound to a reference parameter
		// materializes as an argument object.
		string slot = AddMatSlot("arg", LowerSlotType(bare));
		string address = NewTemp();
		Emit(address + " = addr $" + slot);
		LowerClosureInit(node, address);
		return address;
	}
	if (node.kind == SN_CONSTRUCTOR_ACTION && bare->kind == TK_CLASS)
	{
		// A class temporary binding a reference parameter: an exact
		// binding materializes as an argument object; a derived
		// temporary adjusts to the base referee (and keeps its own
		// dispatch region while live cleanups may unwind past it).
		TypePtr made = RemoveTopCv(node.type);
		bool exact = TypeEquals(made, bare);
		if (!exact && eh_armed_ && !eh_open_ && !in_cleanup_emission_ &&
		    !suppress_eh_regions_)
			OpenEhRegion();
		string address =
			MaterializeTemporary(node, exact ? "arg" : "tmpobj",
			                     true);
		if (!exact && made->kind == TK_CLASS)
		{
			address = AdjustToBase(address, made->named,
			                       bare->named);
		}
		return address;
	}
	if (node.kind == SN_STATEMENT_EXPRESSION)
		return LowerStatementExpressionReference(node, referee);
	TypePtr source = RemoveTopCv(StripRef(node.type));
	bool binds_directly = node.category != VC_PRVALUE &&
		(TypeEquals(source, bare) ||
		 (source->kind == TK_CLASS && bare->kind == TK_CLASS &&
		  BaseClassDistance(source->named, bare->named) >= 0));
	if (!binds_directly && node.category != VC_PRVALUE &&
	    source->kind == TK_CLASS && bare->kind == TK_CLASS &&
	    source->named && source->named->class_record)
	{
		// PA29: a shared virtual-base referee also binds the glvalue
		// directly; AdjustToBase projects the complete-object offset.
		unsigned long long complete = 0;
		if (CompleteObjectOffset(*source->named->class_record,
		                         bare->named, complete))
			binds_directly = true;
	}
	if (binds_directly)
	{
		// A reference-cast class call result has no address of its
		// own; the xvalue materializes its temporary (5.2.9p4).
		// Reference-returning calls keep their direct address.
		bool value_call = false;
		if (node.kind == SN_CALL_EXPRESSION &&
		    source->kind == TK_CLASS && !node.children.empty())
		{
			TypePtr through = NodeType(*node.children[0]);
			if (through->kind == TK_POINTER)
				through = through->target;
			if (through->kind == TK_FUNCTION)
				value_call = !IsReferenceType(through->target);
		}
		string address = value_call
			? MaterializeClassResult(node, "refcall", "")
			: LowerAddressExpr(node);
		if (source->kind == TK_CLASS && bare->kind == TK_CLASS)
		{
			address = AdjustToBase(address, source->named,
			                       bare->named);
		}
		return address;
	}
	if (source->kind == TK_CLASS && bare->kind == TK_CLASS)
	{
		// A class prvalue (call result, conditional) binding a
		// reference: materialize the result object. As with
		// constructor-action temporaries above, an exact binding is an
		// argument object; a derived temporary is a plain temporary
		// adjusted to the base referee.
		bool exact = TypeEquals(source, bare);
		string address =
			MaterializeClassResult(node, exact ? "arg" : "tmpobj", "");
		return AdjustToBase(address, source->named, bare->named);
	}
	// 8.5.3p5: materialize a temporary with the converted value. A
	// function source stores its plain address (the reference spells
	// no decay for the bound temporary).
	string slot = AddMatSlot("refarg", LowerSlotType(bare));
	string value = NodeType(node)->kind == TK_FUNCTION &&
	        bare->kind == TK_POINTER
		? LowerAddressExpr(node)
		: LowerValueAs(node, bare, LCC_INIT);
	Emit("store " + LowerValueType(bare) + " " + value + ", $" + slot);
	string temp = NewTemp();
	Emit(temp + " = addr $" + slot);
	return temp;
}
