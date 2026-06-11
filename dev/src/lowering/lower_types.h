#pragma once

#include <string>

using std::string;

#include "sema/type.h"

// PA14 centralized C++ -> LowIR type facts: one place decides spelling,
// signedness, and scalar conversion operators so every lowering site
// agrees with the PA13 LowIR contract.

// Value spelling: i8/u8/i16/u16/i32/u32/i64/f32/f64/f80/ptr/void.
// Enumerations use the signed iN spelling of their underlying width;
// references, pointers, nullptr_t, and function values are ptr; 64-bit
// unsigned integers spell i64 (LowIR has no u64).
string LowerValueType(const TypePtr& type);

// Slot/parameter storage spelling: like LowerValueType, but arrays
// become obj<sizexalign> addressable storage.
string LowerSlotType(const TypePtr& type);

// True when LowIR operations over the type use the unsigned operator
// and predicate forms (udiv/umod/ushr/ult/ule/ugt/uge).
bool LowerUnsignedOps(const TypePtr& type);

bool LowerFloatType(const TypePtr& type);

// The zero literal compared against in float truth tests.
string LowerFloatZero(const TypePtr& type);

// The convert-instruction operator from `from` to `to`
// (sext/zext/trunc/sitofp/uitofp/fptosi/fptoui/fpext/fptrunc), or ""
// when the conversion is width-preserving (a re-spelling copy).
string LowerConvertOp(const TypePtr& from, const TypePtr& to);

// Object size in bytes of the lowered scalar value.
unsigned long long LowerValueWidth(const TypePtr& type);
