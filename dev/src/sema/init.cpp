#include "sema/init.h"

#include <stdexcept>

using std::runtime_error;

namespace {

// 8.5.2p1: the string-literal kinds permitted for each array element
// type (narrow strings also initialize signed/unsigned char arrays).
bool StringInitializesElement(EFundamentalType literal,
                              EFundamentalType element)
{
	if (literal == FT_CHAR)
		return element == FT_CHAR || element == FT_SIGNED_CHAR ||
			element == FT_UNSIGNED_CHAR;
	return literal == element;
}

// 8.5.2: character array initialized by a string literal.
void InitializeArrayFromString(ImageSlot& slot, const Expr& init,
                               const string& name)
{
	const ImageSlot* literal =
		init.object && init.object->kind == SLOT_STRING ? init.object : 0;
	if (init.category != XC_LVALUE || !literal)
		throw runtime_error("array `" + name +
		                    "` requires a string literal initializer");
	const TypePtr& element = slot.type->target;
	if (element->kind != TK_FUNDAMENTAL ||
	    !StringInitializesElement(literal->type->target->fundamental,
	                              element->fundamental))
		throw runtime_error("string literal cannot initialize the "
		                    "elements of `" + name + "`");
	unsigned long long count = literal->type->bound;
	if (!slot.type->bound_known)
		slot.type = MakeArrayType(element, true, count);  // 8.5.2p1
	else if (count > slot.type->bound)
		throw runtime_error("string literal is too long for `" + name +
		                    "`");
	slot.init_kind = INIT_BYTES;
	slot.init_bytes = literal->init_bytes;
	slot.init_bytes.resize(TypeSize(slot.type), '\0');  // 8.5.2p3
	slot.init_is_constant = true;
}

// Records a converted scalar prvalue as the slot's initial value.
void RecordScalarValue(ImageSlot& slot, const Expr& converted)
{
	switch (converted.value.kind)
	{
	case CK_ADDRESS:
		slot.init_kind = INIT_ADDRESS;
		slot.init_target = converted.value.target;
		slot.init_addend = converted.value.addend;
		slot.init_is_constant = true;
		break;
	case CK_INT:
	case CK_FLOAT:
	case CK_NULL:
		slot.init_kind = INIT_BYTES;
		slot.init_bytes = EncodeConstant(RemoveTopCv(slot.type),
		                                 converted.value);
		slot.init_is_constant = true;
		break;
	case CK_NONCONST:
		// Well-formed dynamic initialization: the mock image holds the
		// zero-initialized state (handout: "otherwise write zero").
		slot.init_kind = INIT_ZERO;
		slot.init_is_constant = false;
		break;
	}
}

// 8.5.3p5: reference binding.
void BindReference(Program& program, ImageSlot& slot, const Expr& init,
                   const string& name)
{
	bool is_rvalue = slot.type->kind == TK_RVALUE_REFERENCE;
	TypePtr referenced = slot.type->target;  // cv1 T1
	Expr src = init;
	if (!src.type && referenced->kind == TK_FUNCTION)
	{
		DeclaredEntity* match = ResolveOverloadSet(src.overloads, referenced);
		if (!match)
			throw runtime_error("no overload of the initializer matches "
			                    "the type of `" + name + "`");
		src = MakeFunctionExpr(vector<DeclaredEntity*>(1, match));
	}
	if (!src.type)
		throw runtime_error("overloaded name cannot initialize `" + name +
		                    "`");
	if (src.category != XC_PRVALUE)
	{
		bool related = TypeEquals(RemoveTopCv(referenced),
		                          RemoveTopCv(src.type));
		bool ref_const, ref_volatile, src_const, src_volatile;
		TopCv(referenced, ref_const, ref_volatile);
		TopCv(src.type, src_const, src_volatile);
		bool compatible = related && (ref_const || !src_const) &&
			(ref_volatile || !src_volatile);
		if (compatible && (!is_rvalue || src.type->kind == TK_FUNCTION))
		{
			// Direct binding to the designated object. An unknown
			// object (through an undefined extern reference) leaves the
			// value unknown - zero in the mock image.
			if (src.object)
			{
				slot.init_kind = INIT_ADDRESS;
				slot.init_target = src.object;
				slot.init_is_constant = true;
			}
			return;
		}
		if (related)
			throw runtime_error(
				is_rvalue
					? "rvalue reference `" + name + "` cannot bind to an "
					  "lvalue"
					: "binding `" + name + "` would lose qualifiers");
	}
	// Lifetime-extended temporary (8.5.3p5 last bullets): only a
	// non-volatile const lvalue reference or an rvalue reference can
	// bind one.
	bool ref_const, ref_volatile;
	TopCv(referenced, ref_const, ref_volatile);
	if (!is_rvalue && !(ref_const && !ref_volatile))
		throw runtime_error("non-const reference `" + name +
		                    "` cannot bind to a temporary");
	ImageSlot* temporary = program.CreateTemporary(referenced);
	ApplyInitializer(program, *temporary, src, name);
	slot.init_kind = INIT_ADDRESS;
	slot.init_target = temporary;
	slot.init_is_constant = true;
}

} // namespace

void ApplyInitializer(Program& program, ImageSlot& slot, const Expr& init,
                      const string& name)
{
	if (IsReferenceType(slot.type))
		BindReference(program, slot, init, name);
	else if (slot.type->kind == TK_ARRAY)
		InitializeArrayFromString(slot, init, name);
	else
	{
		Expr converted;
		if (!ConvertToType(slot.type, init, program.current_tu(),
		                   converted))
			throw runtime_error("cannot convert initializer of `" + name +
			                    "` to " + DescribeType(slot.type));
		RecordScalarValue(slot, converted);
	}
	if (slot.is_constexpr && !slot.init_is_constant)
		throw runtime_error("constexpr variable `" + name +
		                    "` requires a constant expression initializer");
}

void ApplyDefaultInitialization(ImageSlot& slot, const string& name)
{
	if (IsReferenceType(slot.type))
		throw runtime_error("reference `" + name +
		                    "` requires an initializer");
	if (slot.is_constexpr)
		throw runtime_error("constexpr variable `" + name +
		                    "` requires an initializer");
	bool is_const, is_volatile;
	TopCv(slot.type, is_const, is_volatile);
	if (is_const)
		throw runtime_error("default initialization of const `" + name +
		                    "`");
	TypeSize(slot.type);  // rejects incomplete object types
	slot.init_kind = INIT_ZERO;
	slot.init_is_constant = false;
}

bool EvaluateStaticAssertCondition(const Expr& expr, int current_tu)
{
	Expr converted;
	if (!ConvertToType(MakeFundamentalType(FT_BOOL), expr, current_tu,
	                   converted))
		throw runtime_error("static_assert condition cannot be converted "
		                    "to bool");
	if (converted.value.kind != CK_INT)
		throw runtime_error("static_assert condition is not a constant "
		                    "expression");
	return converted.value.int_value != 0;
}

unsigned long long EvaluateArrayBound(const Expr& expr, int current_tu)
{
	if (!expr.type)
		throw runtime_error("array bound is not an integral constant");
	Expr value = expr;
	if (value.category != XC_PRVALUE)
	{
		if (value.type->kind == TK_ARRAY ||
		    value.type->kind == TK_FUNCTION)
			throw runtime_error("array bound is not an integral constant");
		Expr loaded;
		loaded.type = RemoveTopCv(value.type);
		loaded.value = ReadLValue(value, current_tu);
		value = loaded;
	}
	// A converted constant expression of size_t permits only integral
	// conversions (5.19p3), and narrowing - a negative value - is
	// ill-formed.
	if (!IsIntegralType(value.type))
		throw runtime_error("array bound is not an integral constant");
	if (value.value.kind != CK_INT)
		throw runtime_error("array bound is not a constant expression");
	if (IsSignedIntegralFundamental(value.type->fundamental) &&
	    static_cast<long long>(value.value.int_value) < 0)
		throw runtime_error("array bound must be positive");
	if (value.value.int_value == 0)
		throw runtime_error("array bound must be greater than zero");
	return value.value.int_value;
}
