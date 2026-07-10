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
	// 4.10p3 / 5.2.9p11: class pointers adjust along the derivation
	// path - toward the base by the path's offset, back toward the
	// derived class by its negation. A null pointer stays null, so a
	// displaced-base adjustment guards the runtime value; offset-0
	// adjustments keep the unguarded single-projection shape (all the
	// PA15-25 programs). Unrelated classes (reinterpret forms) copy.
	if (source->kind == TK_POINTER &&
	    source->target->kind == TK_CLASS &&
	    target->target->kind == TK_CLASS &&
	    source->target->named != target->target->named)
	{
		const NamedTypeInfo* from = source->target->named;
		const NamedTypeInfo* to = target->target->named;
		int hops = 0;
		unsigned long long offset = 0;
		if (BaseSubobjectPath(from, to, hops, offset) == BP_UNIQUE)
		{
			if (!offset)
				value.text = AdjustToBaseHops(value.text, hops, 0);
			else if (value.known_nonnull)
				value.text = AdjustToBaseHops(value.text, hops, offset);
			else
				value.text =
					AdjustPointerGuarded(value.text, (long long)offset);
		}
		else if (BaseSubobjectPath(to, from, hops, offset) ==
		             BP_UNIQUE && offset)
		{
			// 5.2.9p11: a downcast view shifts back toward the derived
			// object; `this` (never null) keeps the direct projection.
			if (value.known_nonnull)
			{
				string shifted = NewTemp();
				Emit(shifted +
				     " = index i8 [projection=base_subobject] " +
				     value.text + ", -" + to_string(offset));
				value.text = shifted;
			}
			else
				value.text = AdjustPointerGuarded(value.text,
				                                  -(long long)offset);
		}
	}
	value.type = target;
	return value;
}

// A runtime class-pointer adjustment across a displaced base: null
// stays null (4.10p3 / 5.2.9p11), so the displacement applies on the
// non-null path only (the PA27 basecast shape: each arm stores the
// result slot).
string FunctionLowerer::AdjustPointerGuarded(const string& value,
                                             long long delta)
{
	// The guard's branches count as unwind-relevant control flow for
	// callers under cleanups (the reference wraps such calls lazily).
	const_cast<LowFunctionInfo&>(info_).guarded_body = true;
	string slot = AddMatSlot("basecast", "ptr");
	string is_null = NewTemp();
	Emit(is_null + " = cmp eq ptr " + value + ", 0");
	string null_label = NewLabel("basecast_null");
	string adjust_label = NewLabel("basecast_adjust");
	string end_label = NewLabel("basecast_end");
	ReferenceLabel(null_label);
	ReferenceLabel(adjust_label);
	Terminate("branch " + is_null + ", ^" + null_label + ", ^" +
	          adjust_label);
	OpenBlock(null_label);
	Emit("store ptr 0, $" + slot);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(adjust_label);
	string adjusted = NewTemp();
	Emit(adjusted + " = index i8 [projection=base_subobject] " + value +
	     ", " + to_string(delta));
	Emit("store ptr " + adjusted + ", $" + slot);
	ReferenceLabel(end_label);
	Terminate("jump ^" + end_label);
	OpenBlock(end_label);
	string result = NewTemp();
	Emit(result + " = load ptr $" + slot);
	return result;
}

LowerValue FunctionLowerer::ConvertValue(LowerValue value,
                                         const TypePtr& dest,
                                         ELowerConvertContext context)
{
	TypePtr target = RemoveTopCv(dest);
	TypePtr source = RemoveTopCv(StripRef(value.type));
	if (target->kind == TK_POINTER || IsNullPtrType(target))
		return ConvertPointerValue(std::move(value), source, target);
	// PA26: member pointer conversions (null, qualification, 4.11p2
	// base-to-derived) keep the value; a data member pointer converted
	// to a derived class at a non-zero base offset adds the offset.
	if (target->kind == TK_MEMBER_POINTER)
	{
		if (value.imm_null && value.text.empty())
			value.text = "nullptr";
		// 4.11p2 function member pointers: the base subobject's offset
		// folds into the value's this-adjustment half (the high 64
		// bits); call sites of displaced-base classes apply it. Null
		// (0) survives unchanged.
		if (source->kind == TK_MEMBER_POINTER &&
		    target->target->kind == TK_FUNCTION &&
		    target->named != source->named && !value.imm_null)
		{
			int hops = 0;
			unsigned long long offset = 0;
			if (BaseSubobjectPath(target->named, source->named, hops,
			                      offset) == BP_UNIQUE && offset)
			{
				string slot = AddMatSlot("pmadj", "i128");
				Emit("store i128 " + value.text + ", $" + slot);
				string is_null = NewTemp();
				Emit(is_null + " = cmp eq i128 " + value.text + ", 0");
				string shift_label = NewLabel("pmadj_shift");
				string end_label = NewLabel("pmadj_end");
				ReferenceLabel(end_label);
				ReferenceLabel(shift_label);
				Terminate("branch " + is_null + ", ^" + end_label +
				          ", ^" + shift_label);
				OpenBlock(shift_label);
				string delta = NewTemp();
				Emit(delta + " = const i64 " + to_string(offset));
				string wide = NewTemp();
				Emit(wide + " = convert zext i128 i64 " + delta);
				string high = NewTemp();
				Emit(high + " = binary shl i128 " + wide + ", 64");
				string adjusted = NewTemp();
				Emit(adjusted + " = binary add i128 " + value.text +
				     ", " + high);
				Emit("store i128 " + adjusted + ", $" + slot);
				ReferenceLabel(end_label);
				Terminate("jump ^" + end_label);
				OpenBlock(end_label);
				value.text = NewTemp();
				Emit(value.text + " = load i128 $" + slot);
			}
		}
		if (source->kind == TK_MEMBER_POINTER &&
		    target->target->kind != TK_FUNCTION &&
		    target->named != source->named && !value.imm_null)
		{
			int hops = 0;
			unsigned long long offset = 0;
			if (BaseSubobjectPath(target->named, source->named, hops,
			                      offset) == BP_UNIQUE && offset)
			{
				// The null value (0) survives the conversion (4.11p2),
				// so the displacement applies on the non-null path.
				string slot = AddMatSlot("pmadj", "i64");
				Emit("store i64 " + value.text + ", $" + slot);
				string is_null = NewTemp();
				Emit(is_null + " = cmp eq i64 " + value.text + ", 0");
				string shift_label = NewLabel("pmadj_shift");
				string end_label = NewLabel("pmadj_end");
				ReferenceLabel(end_label);
				ReferenceLabel(shift_label);
				Terminate("branch " + is_null + ", ^" + end_label +
				          ", ^" + shift_label);
				OpenBlock(shift_label);
				string temp = NewTemp();
				Emit(temp + " = binary add i64 " + value.text + ", " +
				     to_string(offset));
				Emit("store i64 " + temp + ", $" + slot);
				ReferenceLabel(end_label);
				Terminate("jump ^" + end_label);
				OpenBlock(end_label);
				value.text = NewTemp();
				Emit(value.text + " = load i64 $" + slot);
				value.imm_int = false;
			}
		}
		value.type = target;
		return value;
	}
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
	// 5.2.10p4: a pointer reinterpreted as a pointer-width integer
	// copies the representation.
	if (source->kind == TK_POINTER || source->kind == TK_FUNCTION)
	{
		string temp = NewTemp();
		Emit(temp + " = copy " + LowerValueType(target) + " " +
		     value.text);
		value.text = temp;
		value.imm_int = false;
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
		// A same-spelling conversion that flips signedness (unsigned
		// long <-> long: both spell i64) changes value semantics and
		// keeps its copy in every context; a same-spelling,
		// same-signedness conversion (int -> wchar_t) is an identity
		// (the reference elides it even when spelled).
		EFundamentalType source_fund = source->kind == TK_ENUM
			? source->named->enum_underlying : source->fundamental;
		EFundamentalType target_fund = target->kind == TK_ENUM
			? target->named->enum_underlying : target->fundamental;
		bool sign_flip =
			IsSignedIntegralFundamental(source_fund) !=
			IsSignedIntegralFundamental(target_fund);
		if (LowerValueType(source) != LowerValueType(target) ||
		    sign_flip || (context == LCC_CAST && !enum_identity &&
		                  LowerUnsignedOps(target) &&
		                  TypeSize(target) == 8))
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

