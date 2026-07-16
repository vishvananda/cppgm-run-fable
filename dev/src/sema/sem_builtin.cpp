#include "sema/sem_binder.h"

#include "sema/class_info.h"

using std::string;

// The lazily declared global-scope builtin functions: unknown-name
// lookup falls back here (PA12 __builtin_strlen and friends; PA29
// adds the float-classification pair, which the lowering expands
// inline - see lowering/lower_arg_bind.cpp).

const ScopeBinding* SemBinder::ResolveBuiltinFunction(const string& name)
{
	TypePtr void_type = MakeFundamentalType(FT_VOID);
	TypePtr size_type = MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	TypePtr byte_ptr = MakePointerType(MakeFundamentalType(FT_VOID),
	                                   false, false);
	TypePtr const_char_ptr = MakePointerType(
		MakeCvQualifiedType(MakeFundamentalType(FT_CHAR), true, false),
		false, false);
	TypePtr type;
	vector<TypePtr> params;
	if (name == "__builtin_strlen")
	{
		params.push_back(const_char_ptr);
		type = MakeFunctionType(size_type, params, false);
	}
	else if (name == "__builtin_memcpy" || name == "__builtin_memmove")
	{
		params.push_back(byte_ptr);
		params.push_back(MakePointerType(
			MakeCvQualifiedType(MakeFundamentalType(FT_VOID), true,
			                    false), false, false));
		params.push_back(size_type);
		type = MakeFunctionType(byte_ptr, params, false);
	}
	else if (name == "__builtin_unreachable")
		type = MakeFunctionType(void_type, params, false);
	else if (name == "__builtin_isnan")
	{
		// PA29: any floating operand arrives through its long double
		// conversion; the lowering expands the classification inline
		// (never a runtime call, so it cannot recurse into itself).
		params.push_back(MakeFundamentalType(FT_LONG_DOUBLE));
		type = MakeFunctionType(MakeFundamentalType(FT_BOOL), params,
		                        false);
	}
	else if (name == "__builtin_nanl")
	{
		// PA29: a default quiet NaN (the tag argument only evaluates);
		// the lowering materializes the constant inline.
		params.push_back(const_char_ptr);
		type = MakeFunctionType(MakeFundamentalType(FT_LONG_DOUBLE),
		                        params, false);
	}
	else
		return 0;
	if (const ScopeBinding* existing =
	        FindOwnBinding(*model_.global(), name))
		return existing;
	ScopeBinding binding;
	binding.kind = SB_FUNCTION;
	binding.name = name;
	binding.type = type;
	binding.c_linkage = true;
	binding.fn_defaults.resize(1);
	binding.fn_deleted.resize(1, false);
	binding.fn_access.resize(1, MA_PUBLIC);
	binding.fn_static.resize(1, false);
	binding.fn_inline_def.resize(1, false);
	binding.fn_adl_only.resize(1, false);
	binding.fn_unwind_no.resize(1, true);
	binding.fn_noexcept_decl.resize(1, true);
	return &AddBinding(*model_.global(), binding);
}
