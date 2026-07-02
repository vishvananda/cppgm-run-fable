#include "lowering/lower_function.h"

#include <stdexcept>

#include "lowering/lower_types.h"
#include "sema/const_expr.h"

using std::runtime_error;
using std::to_string;

// PA14 value conversions of the expression lowering: bool tests,
// integral immediates, pointer adjustments, and the spelled
// convert/copy instructions of typed value changes.

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

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

// 4.12 contextual conversion of a non-bool value to bool.
LowerValue FunctionLowerer::ConvertToBool(LowerValue value,
                                          const TypePtr& source,
                                          const TypePtr& target)
{
	if (value.imm_int)
	{
		value.value = ConstValue(FT_BOOL, value.value.bits != 0);
		value.text = RenderConstValue(value.value);
		value.type = target;
		return value;
	}
	if (value.imm_null)
	{
		value.imm_null = false;
		value.imm_int = true;
		value.value = ConstValue(FT_BOOL, 0);
		value.text = "0";
		value.type = target;
		return value;
	}
	string temp = NewTemp();
	if (LowerFloatType(source))
		Emit(temp + " = cmp ne " + LowerValueType(source) + " " +
		     value.text + ", " + LowerFloatZero(source));
	else if (source->kind == TK_POINTER)
	{
		// A pointer compares in its own value space; the bool value
		// then materializes in the bool representation.
		Emit(temp + " = cmp ne ptr " + value.text + ", 0");
		string as_bool = NewTemp();
		Emit(as_bool + " = copy u8 " + temp);
		temp = as_bool;
	}
	else
		Emit(temp + " = cmp ne i64 " + value.text + ", 0");
	value.text = temp;
	value.imm_int = false;
	value.imm_float = false;
	value.type = target;
	return value;
}

// Integral immediates canonicalize to the converted spelling except
// for widening conversions that also change signedness (the oracle
// keeps those spelled, e.g. int -> unsigned long; narrowing ones
// fold, e.g. int -> unsigned char).
LowerValue FunctionLowerer::ConvertIntegralImmediate(LowerValue value,
                                                     const TypePtr& source,
                                                     const TypePtr& target)
{
	EFundamentalType target_fund = target->kind == TK_ENUM
		? target->named->enum_underlying : target->fundamental;
	EFundamentalType source_fund = source->kind == TK_ENUM
		? source->named->enum_underlying : source->fundamental;
	bool widening = LowerValueWidth(source) < LowerValueWidth(target);
	bool sign_change = IsSignedIntegralFundamental(source_fund) !=
		IsSignedIntegralFundamental(target_fund);
	if (widening && sign_change && source_fund != FT_BOOL)
	{
		string temp = NewTemp();
		Emit(temp + " = convert " + LowerConvertOp(source, target) +
		     " " + LowerValueType(target) + " " +
		     LowerValueType(source) + " " + value.text);
		value.text = temp;
		value.imm_int = false;
	}
	else
	{
		value.value = ConvertConstValue(value.value, target_fund);
		value.text = RenderConstValue(value.value);
	}
	value.type = target;
	return value;
}

string FunctionLowerer::LowerValueAs(const SemNode& node,
                                     const TypePtr& dest,
                                     ELowerConvertContext context)
{
	TypePtr target = RemoveTopCv(dest);
	TypePtr source = NodeType(node);
	if (target->kind == TK_POINTER &&
	    (source->kind == TK_ARRAY || source->kind == TK_FUNCTION))
		return LowerPointerOperand(node);
	LowerValue value = LowerValueExpr(node);
	return ConvertValue(value, target, context).text;
}

// Conversions into pointer (and nullptr_t) destinations: null
// materialization, integral reinterpretation, and derived-to-base
// pointer adjustment.
LowerValue FunctionLowerer::ConvertPointerValue(LowerValue value,
                                                const TypePtr& source,
                                                const TypePtr& target)
{
	if (value.imm_null)
	{
		if (value.text.empty())
			value.text = MaterializeNull();
		value.imm_null = false;
		value.type = target;
		return value;
	}
	if (IsIntegralType(source))
	{
		// 5.2.10p5: an integral value reinterpreted as a pointer; a
		// zero immediate stays the null spelling.
		if (!(value.imm_int && value.value.bits == 0))
		{
			string temp = NewTemp();
			Emit(temp + " = copy ptr " + value.text);
			value.text = temp;
		}
		value.imm_int = false;
		value.type = target;
		return value;
	}
	// 4.10p3: a derived-class pointer adjusts to the base subobject
	// (offset 0 in the single-inheritance model).
	if (source->kind == TK_POINTER &&
	    source->target->kind == TK_CLASS &&
	    target->target->kind == TK_CLASS)
	{
		value.text = AdjustToBase(
			value.text,
			BaseClassDistance(source->target->named,
			                  target->target->named));
	}
	value.type = target;
	return value;
}

LowerValue FunctionLowerer::ConvertValue(LowerValue value,
                                         const TypePtr& dest,
                                         ELowerConvertContext context)
{
	TypePtr target = RemoveTopCv(dest);
	TypePtr source = RemoveTopCv(StripRef(value.type));
	if (target->kind == TK_POINTER || IsNullPtrType(target))
		return ConvertPointerValue(std::move(value), source, target);
	bool to_bool = target->kind == TK_FUNDAMENTAL &&
		target->fundamental == FT_BOOL;
	bool from_bool = source->kind == TK_FUNDAMENTAL &&
		source->fundamental == FT_BOOL;
	if (to_bool && !from_bool)
		return ConvertToBool(value, source, target);
	bool float_involved = LowerFloatType(source) ||
		LowerFloatType(target);
	if (float_involved)
	{
		string op = LowerConvertOp(source, target);
		if (!op.empty())
		{
			string temp = NewTemp();
			Emit(temp + " = convert " + op + " " +
			     LowerValueType(target) + " " +
			     LowerValueType(source) + " " + value.text);
			value.text = temp;
			value.imm_int = false;
			value.imm_float = false;
		}
		else if (context == LCC_CAST)
		{
			string temp = NewTemp();
			Emit(temp + " = copy " + LowerValueType(target) + " " +
			     value.text);
			value.text = temp;
		}
		value.type = target;
		return value;
	}
	// Integral (and enumeration) conversions. An identity cast emits
	// nothing (the canonical reference shape).
	if (context == LCC_CAST && TypeEquals(source, target))
	{
		value.type = target;
		return value;
	}
	if (value.imm_int)
		return ConvertIntegralImmediate(value, source, target);
	string op = LowerConvertOp(source, target);
	if (!op.empty())
	{
		string temp = NewTemp();
		Emit(temp + " = convert " + op + " " + LowerValueType(target) +
		     " " + LowerValueType(source) + " " + value.text);
		value.text = temp;
	}
	else
	{
		// An enumeration cast to exactly its underlying type (or back)
		// is an identity and emits nothing; every other spelled cast
		// copies (the canonical reference shapes).
		bool enum_identity = false;
		if (source->kind == TK_ENUM && target->kind == TK_FUNDAMENTAL)
			enum_identity =
				source->named->enum_underlying == target->fundamental;
		else if (target->kind == TK_ENUM &&
		         source->kind == TK_FUNDAMENTAL)
			enum_identity =
				target->named->enum_underlying == source->fundamental;
		else if (source->kind == TK_ENUM && target->kind == TK_ENUM)
			enum_identity = source->named->enum_underlying ==
				target->named->enum_underlying;
		// An 8-byte unsigned destination spells i64 (LowIR has no
		// u64); the reference keeps the spelled cast copy there.
		if (enum_identity && LowerUnsignedOps(target) &&
		    TypeSize(target) == 8)
			enum_identity = false;
		if (LowerValueType(source) != LowerValueType(target) ||
		    (context == LCC_CAST && !enum_identity))
		{
		string temp = NewTemp();
		Emit(temp + " = copy " + LowerValueType(target) + " " +
		     value.text);
		value.text = temp;
		}
	}
	value.type = target;
	return value;
}

