#include "lowering/lower_types.h"

#include <stdexcept>

#include "post_token.h"

using std::runtime_error;
using std::to_string;

namespace {

runtime_error OutsideBoundary(const char* what)
{
	return runtime_error(string(what) +
	                     " is outside the PA14 assignment boundary");
}

// The signed iN spelling of a byte width (enums and similar).
string SignedSpelling(unsigned long long bytes)
{
	switch (bytes)
	{
	case 1: return "i8";
	case 2: return "i16";
	case 4: return "i32";
	case 8: return "i64";
	}
	throw OutsideBoundary("integral width");
}

string FundamentalSpelling(EFundamentalType type)
{
	switch (type)
	{
	case FT_VOID: return "void";
	case FT_BOOL: return "u8";
	case FT_CHAR:
	case FT_SIGNED_CHAR: return "i8";
	case FT_UNSIGNED_CHAR: return "u8";
	case FT_SHORT_INT: return "i16";
	case FT_UNSIGNED_SHORT_INT:
	case FT_CHAR16_T: return "u16";
	case FT_INT:
	case FT_WCHAR_T: return "i32";
	case FT_UNSIGNED_INT:
	case FT_CHAR32_T: return "u32";
	case FT_LONG_INT:
	case FT_LONG_LONG_INT:
	// LowIR has no u64; 64-bit unsigned spells i64 and the operator
	// forms carry the signedness.
	case FT_UNSIGNED_LONG_INT:
	case FT_UNSIGNED_LONG_LONG_INT: return "i64";
	case FT_FLOAT: return "f32";
	case FT_DOUBLE: return "f64";
	case FT_LONG_DOUBLE: return "f80";
	case FT_NULLPTR_T: return "ptr";
	}
	throw OutsideBoundary("fundamental type");
}

// The integer signedness/width view of an integral or enumeration
// type; throws for anything else.
void IntegerFacts(const TypePtr& type, bool& is_signed,
                  unsigned long long& bytes)
{
	if (type->kind == TK_ENUM)
	{
		is_signed = IsSignedIntegralFundamental(
			type->named->enum_underlying);
		bytes = TypeSize(type);
		return;
	}
	if (type->kind != TK_FUNDAMENTAL ||
	    !IsIntegralFundamental(type->fundamental))
		throw OutsideBoundary("integer conversion operand");
	is_signed = IsSignedIntegralFundamental(type->fundamental);
	bytes = TypeSize(type);
}

}  // namespace

string LowerValueType(const TypePtr& type)
{
	switch (type->kind)
	{
	case TK_FUNDAMENTAL:
		return FundamentalSpelling(type->fundamental);
	case TK_POINTER:
	case TK_LVALUE_REFERENCE:
	case TK_RVALUE_REFERENCE:
	case TK_FUNCTION:
		return "ptr";
	case TK_ENUM:
		return SignedSpelling(TypeSize(type));
	default:
		throw OutsideBoundary("value type");
	}
}

string LowerSlotType(const TypePtr& type)
{
	if (type->kind == TK_ARRAY || type->kind == TK_CLASS)
		return "obj<" + to_string(TypeSize(type)) + "x" +
			to_string(TypeAlignment(type)) + ">";
	return LowerValueType(type);
}

bool LowerUnsignedOps(const TypePtr& type)
{
	if (type->kind == TK_POINTER)
		return true;
	if (type->kind == TK_ENUM)
		return !IsSignedIntegralFundamental(type->named->enum_underlying);
	if (type->kind != TK_FUNDAMENTAL)
		return false;
	return IsIntegralFundamental(type->fundamental) &&
		!IsSignedIntegralFundamental(type->fundamental);
}

bool LowerFloatType(const TypePtr& type)
{
	return type->kind == TK_FUNDAMENTAL &&
		IsFloatingFundamental(type->fundamental);
}

string LowerFloatZero(const TypePtr& type)
{
	if (type->fundamental == FT_FLOAT)
		return "0.0f";
	if (type->fundamental == FT_LONG_DOUBLE)
		return "0.0L";
	return "0.0";
}

string LowerConvertOp(const TypePtr& from, const TypePtr& to)
{
	bool from_float = LowerFloatType(from);
	bool to_float = LowerFloatType(to);
	if (from_float && to_float)
	{
		unsigned long long fw = TypeSize(from);
		unsigned long long tw = TypeSize(to);
		if (fw == tw)
			return "";
		return tw > fw ? "fpext" : "fptrunc";
	}
	if (from_float)
	{
		bool to_signed;
		unsigned long long to_bytes;
		IntegerFacts(to, to_signed, to_bytes);
		return to_signed ? "fptosi" : "fptoui";
	}
	if (to_float)
	{
		bool from_signed;
		unsigned long long from_bytes;
		IntegerFacts(from, from_signed, from_bytes);
		return from_signed ? "sitofp" : "uitofp";
	}
	bool from_signed;
	bool to_signed;
	unsigned long long from_bytes;
	unsigned long long to_bytes;
	IntegerFacts(from, from_signed, from_bytes);
	IntegerFacts(to, to_signed, to_bytes);
	if (from_bytes == to_bytes)
		return "";
	if (to_bytes < from_bytes)
		return "trunc";
	return from_signed ? "sext" : "zext";
}

unsigned long long LowerValueWidth(const TypePtr& type)
{
	if (type->kind == TK_FUNCTION)
		return 8;
	return TypeSize(type);
}
