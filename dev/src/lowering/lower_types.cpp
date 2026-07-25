#include "lowering/lower_types.h"

#include <stdexcept>

#include "post_token.h"
#include "sema/class_info.h"

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
	// Likewise one i128 spelling for both 128-bit types.
	case FT_INT128:
	case FT_UINT128: return "i128";
	case FT_FLOAT: return "f32";
	case FT_DOUBLE: return "f64";
	case FT_LONG_DOUBLE: return "f80";
	case FT_NULLPTR_T: return "i64";
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
	case TK_MEMBER_POINTER:
		// PA26 Itanium-style values with 0 as null: a data member
		// pointer is the field offset + 1; a member function pointer
		// packs {address, this-adjustment} into an i128.
		return type->target->kind == TK_FUNCTION ? "i128" : "i64";
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

// --- PA16 object ABI ---------------------------------------------------

bool LowerClassDirect(const TypePtr& bare, bool host_abi)
{
	if (!bare->named || !bare->named->class_record)
		return false;
	return host_abi ? ClassParamDirectHost(*bare->named->class_record)
	                : ClassParamDirect(*bare->named->class_record);
}

bool LowerClassReturnDirect(const TypePtr& bare, bool host_abi)
{
	if (!bare->named || !bare->named->class_record)
		return false;
	return host_abi
		? ClassReturnDirectHost(*bare->named->class_record)
		: ClassReturnDirect(*bare->named->class_record);
}

string LowerObjSpan(const TypePtr& bare)
{
	return to_string(TypeSize(bare)) + "x" + to_string(TypeAlignment(bare));
}

namespace {

bool ClassFieldsAllInteger(const ClassInfo& record);

// SysV AMD64 3.2.3: whether this leaf type classifies INTEGER (no
// float/double/long-double anywhere below it). Pointers, enums, and
// member pointers are INTEGER.
bool TypeIsIntegerClass(const TypePtr& type)
{
	TypePtr bare = RemoveTopCv(type);
	while (bare->kind == TK_ARRAY)
		bare = RemoveTopCv(bare->target);
	if (LowerFloatType(bare))
		return false;
	if (bare->kind == TK_CLASS)
		return bare->named && bare->named->class_record &&
			ClassFieldsAllInteger(*bare->named->class_record);
	return true;
}

bool ClassFieldsAllInteger(const ClassInfo& record)
{
	for (size_t b = 0; b < record.direct_bases.size(); b++)
		if (!ClassFieldsAllInteger(*record.direct_bases[b].cls))
			return false;
	for (size_t f = 0; f < record.fields.size(); f++)
		if (!TypeIsIntegerClass(record.fields[f].type))
			return false;
	return true;
}

}  // namespace

void LowerAbiParameter(const TypePtr& param, string& type_text,
                       string& pass, bool host_abi)
{
	pass.clear();
	if (IsReferenceType(param))
	{
		type_text = "ptr";
		pass = "reference";
		return;
	}
	TypePtr bare = RemoveTopCv(param);
	if (bare->kind == TK_CLASS)
	{
		if (LowerClassDirect(bare, host_abi))
		{
			type_text = LowerSlotType(bare);
			// PA33 host ABI: a 9..16-byte all-INTEGER object passes in
			// two GPRs (SysV two-eightbyte classification). Mode-gated
			// so whole-program LowIR shapes stay pinned; SSE-classified
			// eightbytes stay on the current memory path (no fixture
			// exercises them yet - see pa33/plan.md).
			// PA37: the serialized LowIR no longer spells gpr_pair; the
			// object layer classifies direct 9..16-byte host objects
			// itself (lowir_to_mir_flow.cpp).
			(void)host_abi;
		}
		else
		{
			type_text = "ptr";
			pass = "by_address";
		}
		return;
	}
	type_text = LowerValueType(param);
}

bool LowerAbiReturn(const TypePtr& return_type, string& ret_text,
                    bool host_abi)
{
	TypePtr bare = RemoveTopCv(return_type);
	if (bare->kind == TK_CLASS)
	{
		if (LowerClassReturnDirect(bare, host_abi))
		{
			ret_text = LowerSlotType(bare);
			return false;
		}
		ret_text = "void";
		return true;
	}
	ret_text = LowerValueType(return_type);
	return false;
}
