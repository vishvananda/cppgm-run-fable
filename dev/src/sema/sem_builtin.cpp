#include "sema/sem_binder.h"

#include "sema/class_info.h"

using std::string;

// The lazily declared global-scope builtin functions: unknown-name
// lookup falls back here (PA12 __builtin_strlen and friends; PA29
// adds the float-classification pair; PA33 the vararg-cursor and
// alloca builtins; PA34 the hosted GNU/Clang builtin families).
//
// Every entry here either expands inline during lowering
// (lowering/lower_builtin.cpp, lowering/lower_expand.cpp) or - for
// the C-library-backed builtins under separate compilation - lowers
// to a call carrying the host C library symbol name
// (lower_unit.cpp HostLibraryBuiltinSymbol). Constant-foldable
// entries evaluate in sema/const_eval_builtin.cpp.

namespace {

// One builtin signature, spelled compactly: ret / params use one
// letter per type (see DecodeBuiltinType).
struct BuiltinFunctionSpec
{
	const char* name;
	char ret;
	const char* params;
	bool variadic;
};

const BuiltinFunctionSpec kBuiltinFunctions[] = {
	// PA12/PA34 C-library-backed builtins (host symbol under
	// separate compilation).
	{"__builtin_strlen", 'm', "s", false},
	{"__builtin_memcpy", 'p', "pqm", false},
	{"__builtin_memmove", 'p', "pqm", false},
	{"__builtin_strcmp", 'i', "ss", false},
	{"__builtin_memcmp", 'i', "qqm", false},
	{"__builtin_memchr", 'p', "qim", false},
	{"__builtin_strchr", 'c', "si", false},
	{"__builtin_bzero", 'v', "pm", false},
	// Control/annotation builtins the lowering erases or reduces to
	// their surviving operand.
	{"__builtin_unreachable", 'v', "", false},
	{"__builtin_expect", 'l', "ll", false},
	{"__builtin_assume_aligned", 'p', "qm", true},
	{"__builtin_prefetch", 'v', "q", true},
	// PA29 float classification; PA34 rounds out the constant and
	// classification families (lowering materializes the constants).
	{"__builtin_isnan", 'b', "e", false},
	{"__builtin_nanl", 'e', "s", false},
	{"__builtin_nan", 'd', "s", false},
	{"__builtin_nanf", 'f', "s", false},
	{"__builtin_nans", 'd', "s", false},
	{"__builtin_nansf", 'f', "s", false},
	{"__builtin_nansl", 'e', "s", false},
	{"__builtin_inf", 'd', "", false},
	{"__builtin_inff", 'f', "", false},
	{"__builtin_infl", 'e', "", false},
	{"__builtin_huge_val", 'd', "", false},
	{"__builtin_huge_valf", 'f', "", false},
	{"__builtin_huge_vall", 'e', "", false},
	// The translation-environment queries (constant per 5.2.4.2.2:
	// this implementation translates under round-to-nearest).
	{"__builtin_flt_rounds", 'i', "", false},
	{"__builtin_is_constant_evaluated", 'b', "", false},
	// PA33 vararg cursor / stack allocation.
	{"__builtin_va_start", 'v', "F", true},
	{"__builtin_va_end", 'v', "F", false},
	{"__builtin_alloca", 'p', "m", false},
	// Integer bit-manipulation family (constant folds in
	// const_eval_builtin.cpp; runtime forms expand inline).
	{"__builtin_bswap16", 'h', "h", false},
	{"__builtin_bswap32", 'u', "u", false},
	{"__builtin_bswap64", 'y', "y", false},
	{"__builtin_clz", 'i', "u", false},
	{"__builtin_clzl", 'i', "m", false},
	{"__builtin_clzll", 'i', "y", false},
	{"__builtin_ctz", 'i', "u", false},
	{"__builtin_ctzl", 'i', "m", false},
	{"__builtin_ctzll", 'i', "y", false},
	{"__builtin_popcount", 'i', "u", false},
	{"__builtin_popcountl", 'i', "m", false},
	{"__builtin_popcountll", 'i', "y", false},
	// C11 fence operators (the LowIR fence instructions).
	{"__c11_atomic_thread_fence", 'v', "i", false},
	{"__c11_atomic_signal_fence", 'v', "i", false},
	{0, 0, 0, false}
};

TypePtr DecodeBuiltinType(char code)
{
	switch (code)
	{
	case 'v': return MakeFundamentalType(FT_VOID);
	case 'b': return MakeFundamentalType(FT_BOOL);
	case 'i': return MakeFundamentalType(FT_INT);
	case 'l': return MakeFundamentalType(FT_LONG_INT);
	case 'm': return MakeFundamentalType(FT_UNSIGNED_LONG_INT);
	case 'y': return MakeFundamentalType(FT_UNSIGNED_LONG_LONG_INT);
	case 'h': return MakeFundamentalType(FT_UNSIGNED_SHORT_INT);
	case 'u': return MakeFundamentalType(FT_UNSIGNED_INT);
	case 'f': return MakeFundamentalType(FT_FLOAT);
	case 'd': return MakeFundamentalType(FT_DOUBLE);
	case 'e': return MakeFundamentalType(FT_LONG_DOUBLE);
	case 'c': return MakePointerType(MakeFundamentalType(FT_CHAR),
	                                 false, false);
	case 's': return MakePointerType(
		MakeCvQualifiedType(MakeFundamentalType(FT_CHAR), true, false),
		false, false);
	case 'p': return MakePointerType(MakeFundamentalType(FT_VOID),
	                                 false, false);
	case 'q': return MakePointerType(
		MakeCvQualifiedType(MakeFundamentalType(FT_VOID), true, false),
		false, false);
	case 'F':
		// The va_list cursor argument, decayed to its element pointer.
		return MakePointerType(
			MakeFundamentalType(FT_UNSIGNED_LONG_INT), false, false);
	}
	return TypePtr();
}

}  // namespace

const ScopeBinding* SemBinder::ResolveBuiltinFunction(const string& name)
{
	const BuiltinFunctionSpec* spec = 0;
	for (const BuiltinFunctionSpec* at = kBuiltinFunctions; at->name; at++)
		if (name == at->name)
		{
			spec = at;
			break;
		}
	if (!spec)
		return 0;
	vector<TypePtr> params;
	for (const char* code = spec->params; *code; code++)
		params.push_back(DecodeBuiltinType(*code));
	TypePtr type = MakeFunctionType(DecodeBuiltinType(spec->ret), params,
	                                spec->variadic);
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
