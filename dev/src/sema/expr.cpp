#include "sema/expr.h"

#include <cmath>
#include <cstring>

using std::memcpy;

namespace {

unsigned long long SignExtend(unsigned long long value, size_t size)
{
	if (size >= 8)
		return value;
	unsigned long long sign_bit = 1ull << (size * 8 - 1);
	if (value & sign_bit)
		value |= ~(sign_bit * 2 - 1);
	return value;
}

long double DecodeFloatBytes(EFundamentalType type, const string& bytes)
{
	if (type == FT_FLOAT)
	{
		float value;
		memcpy(&value, bytes.data(), sizeof(value));
		return value;
	}
	if (type == FT_DOUBLE)
	{
		double value;
		memcpy(&value, bytes.data(), sizeof(value));
		return value;
	}
	long double value = 0.0L;
	memcpy(&value, bytes.data(), 10);  // x87: 10 value bytes, 6 padding
	return value;
}

// The value of fundamental/pointer object-representation bytes. The
// only pointer/nullptr_t values stored as bytes are null.
ConstValue DecodeBytes(const TypePtr& type, const string& bytes)
{
	ConstValue value;
	if (type->kind == TK_POINTER || IsNullPtrType(type))
	{
		value.kind = CK_NULL;
		return value;
	}
	if (IsIntegralFundamental(type->fundamental))
	{
		value.kind = CK_INT;
		value.int_value = LittleEndianValue(bytes);
		if (IsSignedIntegralFundamental(type->fundamental))
			value.int_value = SignExtend(value.int_value, bytes.size());
		return value;
	}
	value.kind = CK_FLOAT;
	value.float_value = DecodeFloatBytes(type->fundamental, bytes);
	return value;
}

ConstValue MakeIntValue(unsigned long long value)
{
	ConstValue result;
	result.kind = CK_INT;
	result.int_value = value;
	return result;
}

ConstValue MakeAddressValue(const ImageSlot* target,
                            unsigned long long addend)
{
	ConstValue result;
	result.kind = CK_ADDRESS;
	result.target = target;
	result.addend = addend;
	return result;
}

// Integral conversions (4.7): modulo 2^N, sign-extended into the
// 64-bit representation when the destination is signed.
unsigned long long ConvertToWidth(unsigned long long value, size_t size,
                                  bool is_signed)
{
	if (size < 8)
		value &= (1ull << (size * 8)) - 1;
	return is_signed ? SignExtend(value, size) : value;
}

// Floating-integral conversion (4.9): truncate toward zero; a value
// outside the destination range is undefined, hence not constant.
bool FloatToIntegral(long double value, size_t size, bool is_signed,
                     unsigned long long& out)
{
	long double truncated = truncl(value);
	if (is_signed)
	{
		long double limit = ldexpl(1.0L, size * 8 - 1);
		if (!(truncated >= -limit && truncated < limit))
			return false;
		out = ConvertToWidth(
			static_cast<unsigned long long>(
				static_cast<long long>(truncated)),
			size, true);
		return true;
	}
	long double limit = ldexpl(1.0L, size * 8);
	if (!(truncated > -1.0L && truncated < limit))
		return false;
	out = static_cast<unsigned long long>(truncated);
	return true;
}

// Arithmetic conversions (4.5-4.9) of a known value.
ConstValue ConvertArithmetic(EFundamentalType dest, EFundamentalType src,
                             const ConstValue& value)
{
	ConstValue result;
	if (value.kind == CK_NONCONST)
		return result;
	if (dest == FT_BOOL)
		return MakeIntValue(value.kind == CK_INT ? value.int_value != 0
		                                         : value.float_value != 0.0L);
	if (IsIntegralFundamental(dest))
	{
		size_t size = TypeSize(MakeFundamentalType(dest));
		bool is_signed = IsSignedIntegralFundamental(dest);
		if (value.kind == CK_INT)
			return MakeIntValue(ConvertToWidth(value.int_value, size,
			                                   is_signed));
		unsigned long long converted;
		if (!FloatToIntegral(value.float_value, size, is_signed, converted))
			return result;
		return MakeIntValue(converted);
	}
	long double wide;
	if (value.kind == CK_INT)
		wide = IsSignedIntegralFundamental(src)
			? static_cast<long double>(
				static_cast<long long>(value.int_value))
			: static_cast<long double>(value.int_value);
	else
		wide = value.float_value;
	if (dest == FT_FLOAT)
		wide = static_cast<float>(wide);
	else if (dest == FT_DOUBLE)
		wide = static_cast<double>(wide);
	result.kind = CK_FLOAT;
	result.float_value = wide;
	return result;
}

} // namespace

DeclaredEntity* ResolveOverloadSet(const vector<DeclaredEntity*>& set,
                                   const TypePtr& function_type)
{
	DeclaredEntity* match = 0;
	for (size_t i = 0; i < set.size(); i++)
	{
		if (!TypeEquals(set[i]->type, function_type))
			continue;
		if (match)
			return 0;
		match = set[i];
	}
	return match;
}

Expr MakeLiteralExpr(const PostToken& token)
{
	Expr expr;
	expr.type = MakeFundamentalType(token.type);
	expr.value = DecodeBytes(expr.type, token.data);
	return expr;
}

Expr MakeBoolLiteralExpr(bool value)
{
	Expr expr;
	expr.type = MakeFundamentalType(FT_BOOL);
	expr.value = MakeIntValue(value ? 1 : 0);
	return expr;
}

Expr MakeNullptrLiteralExpr()
{
	Expr expr;
	expr.type = MakeFundamentalType(FT_NULLPTR_T);
	expr.value.kind = CK_NULL;
	return expr;
}

Expr MakeStringLiteralExpr(const ImageSlot* slot)
{
	Expr expr;
	expr.category = XC_LVALUE;
	expr.type = slot->type;
	expr.object = slot;
	expr.value = MakeAddressValue(slot, 0);
	return expr;
}

Expr MakeVariableExpr(const DeclaredEntity* entity, int current_tu)
{
	Expr expr;
	expr.category = XC_LVALUE;
	if (IsReferenceType(entity->type))
	{
		// References are implicitly dereferenced; the designated object
		// is known only if this translation unit saw a constant binding
		// (5.19's reference rule).
		expr.type = entity->type->target;
		if (entity->defined_tu == current_tu && entity->init_is_constant &&
		    entity->init_kind == INIT_ADDRESS)
		{
			expr.object = entity->init_target;
			expr.value = MakeAddressValue(entity->init_target,
			                              entity->init_addend);
		}
		return expr;
	}
	expr.type = entity->type;
	expr.object = entity;
	expr.value = MakeAddressValue(entity, 0);
	return expr;
}

Expr MakeFunctionExpr(const vector<DeclaredEntity*>& overloads)
{
	Expr expr;
	expr.category = XC_LVALUE;
	expr.overloads = overloads;
	if (overloads.size() == 1)
	{
		expr.type = overloads[0]->type;
		expr.object = overloads[0];
		expr.value = MakeAddressValue(overloads[0], 0);
	}
	return expr;
}

bool IsNullPointerConstant(const Expr& expr)
{
	if (!expr.type)
		return false;
	if (IsNullPtrType(expr.type))
		return true;
	return IsIntegralType(expr.type) && expr.value.kind == CK_INT &&
		expr.value.int_value == 0;
}

ConstValue ReadLValue(const Expr& expr, int current_tu)
{
	ConstValue nonconst;
	const ImageSlot* object = expr.object;
	if (!object || object->kind == SLOT_FUNCTION ||
	    object->kind == SLOT_STRING)
		return nonconst;
	if (expr.value.kind == CK_ADDRESS && expr.value.addend != 0)
		return nonconst;
	const TypePtr& type = object->type;
	if (type->kind == TK_ARRAY || type->is_volatile)
		return nonconst;
	bool readable = object->is_constexpr ||
		(type->is_const && IsIntegralType(type));
	if (!readable || object->defined_tu != current_tu ||
	    !object->init_is_constant)
		return nonconst;
	switch (object->init_kind)
	{
	case INIT_BYTES:
		return DecodeBytes(type, object->init_bytes);
	case INIT_ADDRESS:
		return MakeAddressValue(object->init_target, object->init_addend);
	case INIT_ZERO:
		break;
	}
	return nonconst;
}

bool ConvertToType(const TypePtr& dest_type, const Expr& src,
                   int current_tu, Expr& out)
{
	TypePtr dest = RemoveTopCv(dest_type);
	Expr value = src;
	if (value.category != XC_PRVALUE)
	{
		if (!value.type)
		{
			// Unresolved overload set: only a pointer-to-function
			// destination selects a candidate (13.4).
			if (dest->kind != TK_POINTER ||
			    dest->target->kind != TK_FUNCTION)
				return false;
			DeclaredEntity* match =
				ResolveOverloadSet(value.overloads, dest->target);
			if (!match)
				return false;
			value = MakeFunctionExpr(vector<DeclaredEntity*>(1, match));
		}
		if (value.type->kind == TK_ARRAY)
		{
			Expr decayed;
			decayed.type = MakePointerType(value.type->target, false, false);
			if (value.object)
				decayed.value = MakeAddressValue(value.object, 0);
			value = decayed;
		}
		else if (value.type->kind == TK_FUNCTION)
		{
			Expr decayed;
			decayed.type = MakePointerType(value.type, false, false);
			if (value.object)
				decayed.value = MakeAddressValue(value.object, 0);
			value = decayed;
		}
		else
		{
			Expr loaded;
			loaded.type = RemoveTopCv(value.type);
			loaded.value = ReadLValue(value, current_tu);
			value = loaded;
		}
	}
	if (TypeEquals(dest, value.type))
	{
		out = value;
		return true;
	}
	if (dest->kind == TK_FUNDAMENTAL)
	{
		if (dest->fundamental == FT_BOOL &&
		    (value.type->kind == TK_POINTER || IsNullPtrType(value.type)))
		{
			// Boolean conversion of a pointer (4.12): every object and
			// function in the image has a nonzero offset.
			out.type = dest;
			if (value.value.kind == CK_ADDRESS)
				out.value = MakeIntValue(1);
			else if (value.value.kind == CK_NULL)
				out.value = MakeIntValue(0);
			else
				out.value = ConstValue();
			return true;
		}
		if (IsArithmeticType(dest) && IsArithmeticType(value.type))
		{
			out.type = dest;
			out.value = ConvertArithmetic(dest->fundamental,
			                              value.type->fundamental,
			                              value.value);
			return true;
		}
		return false;
	}
	if (dest->kind == TK_POINTER)
	{
		if (IsNullPointerConstant(value))
		{
			out.type = dest;
			out.value = ConstValue();
			out.value.kind = CK_NULL;
			return true;
		}
		if (value.type->kind != TK_POINTER)
			return false;
		if (QualificationConvertible(value.type, dest))
		{
			out = value;
			out.type = dest;
			return true;
		}
		// 4.10p2: pointer to object -> pointer to (no less cv) void.
		if (IsVoidType(dest->target) &&
		    value.type->target->kind != TK_FUNCTION &&
		    !IsVoidType(value.type->target) &&
		    (!value.type->target->is_const || dest->target->is_const) &&
		    (!value.type->target->is_volatile || dest->target->is_volatile))
		{
			out = value;
			out.type = dest;
			return true;
		}
		return false;
	}
	return false;
}

string EncodeConstant(const TypePtr& type, const ConstValue& value)
{
	if (value.kind == CK_NULL)
		return string(TypeSize(type), '\0');
	if (value.kind == CK_FLOAT)
	{
		if (type->fundamental == FT_FLOAT)
		{
			float narrowed = static_cast<float>(value.float_value);
			string bytes(sizeof(narrowed), '\0');
			memcpy(&bytes[0], &narrowed, sizeof(narrowed));
			return bytes;
		}
		if (type->fundamental == FT_DOUBLE)
		{
			double narrowed = static_cast<double>(value.float_value);
			string bytes(sizeof(narrowed), '\0');
			memcpy(&bytes[0], &narrowed, sizeof(narrowed));
			return bytes;
		}
		// x87 long double: 10 value bytes, then zero padding (the ABI
		// leaves them unspecified; zeros keep the image deterministic).
		string bytes(16, '\0');
		memcpy(&bytes[0], &value.float_value, 10);
		return bytes;
	}
	return LittleEndianBytes(value.int_value, TypeSize(type));
}
