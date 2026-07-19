#include "hosted_probes.h"

#include <set>

using std::set;
using std::string;

namespace {

// clang spelling equivalence: __name__ queries the same capability as
// name (both for features and attributes).
string StripUnderscoreDecoration(const string & name)
{
	if (name.size() > 4 &&
	    name.compare(0, 2, "__") == 0 &&
	    name.compare(name.size() - 2, 2, "__") == 0)
		return name.substr(2, name.size() - 4);
	return name;
}

const char * const kSupportedBuiltins[] = {
	// builtin functions with sema declarations and lowering support
	"__builtin_strlen",
	"__builtin_memcpy",
	"__builtin_memmove",
	"__builtin_strcmp",
	"__builtin_memcmp",
	"__builtin_memchr",
	"__builtin_strchr",
	"__builtin_bzero",
	"__builtin_unreachable",
	"__builtin_expect",
	"__builtin_assume_aligned",
	"__builtin_prefetch",
	"__builtin_constant_p",
	"__builtin_isnan",
	"__builtin_nan",
	"__builtin_nanf",
	"__builtin_nanl",
	"__builtin_nans",
	"__builtin_nansf",
	"__builtin_nansl",
	"__builtin_inf",
	"__builtin_inff",
	"__builtin_infl",
	"__builtin_huge_val",
	"__builtin_huge_valf",
	"__builtin_huge_vall",
	"__builtin_fpclassify",
	"__builtin_flt_rounds",
	"__builtin_is_constant_evaluated",
	"__builtin_va_start",
	"__builtin_va_end",
	"__builtin_va_arg",
	"__builtin_alloca",
	"__builtin_addressof",
	"__builtin_offsetof",
	"__builtin_invoke",
	"__builtin_complex",
	"__builtin_operator_new",
	"__builtin_operator_delete",
	"__builtin_add_overflow",
	"__builtin_sub_overflow",
	"__builtin_mul_overflow",
	"__builtin_bswap16",
	"__builtin_bswap32",
	"__builtin_bswap64",
	"__builtin_clz",
	"__builtin_clzl",
	"__builtin_clzll",
	"__builtin_clzg",
	"__builtin_ctz",
	"__builtin_ctzl",
	"__builtin_ctzll",
	"__builtin_ctzg",
	"__builtin_popcount",
	"__builtin_popcountl",
	"__builtin_popcountll",
	"__builtin_popcountg",
	"__c11_atomic_thread_fence",
	"__c11_atomic_signal_fence",
	"__c11_atomic_init",
	"__c11_atomic_load",
	"__c11_atomic_store",
	"__c11_atomic_exchange",
	"__c11_atomic_compare_exchange_strong",
	"__c11_atomic_compare_exchange_weak",
	"__atomic_load",
	"__atomic_load_n",
	"__atomic_store",
	"__atomic_store_n",
	"__atomic_exchange_n",
	"__atomic_add_fetch",
	"__atomic_sub_fetch",
	"__atomic_fetch_add",
	"__atomic_fetch_sub",
	"__atomic_always_lock_free",
	"__atomic_is_lock_free",
	"__c11_atomic_is_lock_free",
	"__c11_atomic_fetch_add",
	"__c11_atomic_fetch_sub",
	"__c11_atomic_fetch_and",
	"__c11_atomic_fetch_or",
	"__c11_atomic_fetch_xor",
	"__atomic_fetch_and",
	"__atomic_fetch_or",
	"__atomic_fetch_xor",
	"__atomic_and_fetch",
	"__atomic_or_fetch",
	"__atomic_xor_fetch",
	"__sync_lock_test_and_set",
	"__sync_lock_release",
	// builtin trait/transform identifiers (clang answers __has_builtin
	// for these; the sema parses them as builtin expressions/types)
	"__integer_pack",
	"__remove_reference_t",
	"__is_trivially_destructible",
	"__reference_constructs_from_temporary",
	"__reference_binds_to_temporary",
	0
};

// The C++11 language feature set this compiler implements (clang
// __has_feature names). Supported features answer both __has_feature
// and __has_extension.
const char * const kSupportedFeatures[] = {
	"cxx_alias_templates",
	"cxx_alignas",
	"cxx_alignof",
	"cxx_atomic",
	"cxx_attributes",
	"cxx_auto_type",
	"cxx_binary_literals",
	"cxx_constexpr",
	"cxx_decltype",
	"cxx_decltype_incomplete_return_types",
	"cxx_default_function_template_args",
	"cxx_defaulted_functions",
	"cxx_delegating_constructors",
	"cxx_deleted_functions",
	"cxx_exceptions",
	"cxx_explicit_conversions",
	"cxx_generalized_initializers",
	"cxx_implicit_moves",
	"cxx_inheriting_constructors",
	"cxx_inline_namespaces",
	"cxx_lambdas",
	"cxx_local_type_template_args",
	"cxx_noexcept",
	"cxx_nonstatic_member_init",
	"cxx_nullptr",
	"cxx_override_control",
	"cxx_range_for",
	"cxx_raw_string_literals",
	"cxx_reference_qualified_functions",
	"cxx_rtti",
	"cxx_rvalue_references",
	"cxx_static_assert",
	"cxx_strong_enums",
	"cxx_trailing_return",
	"cxx_unicode_literals",
	"cxx_unrestricted_unions",
	"cxx_user_literals",
	"cxx_variable_templates",
	"cxx_variadic_templates",
	0
};

// Builtin type transforms the sema type builder evaluates
// (type_builder.cpp ResolveTransformSpecifier).
const char * const kBuiltinTransforms[] = {
	"__decay",
	"__remove_reference_t",
	"__remove_reference",
	"__remove_const",
	"__remove_volatile",
	"__remove_cv",
	"__remove_cvref",
	"__remove_extent",
	"__remove_all_extents",
	"__remove_pointer",
	"__add_pointer",
	"__add_lvalue_reference",
	"__add_rvalue_reference",
	"__underlying_type",
	0
};

// Builtin type traits the sema evaluates (sema/sem_trait.cpp).
const char * const kBuiltinTraits[] = {
	"__is_same",
	"__is_void",
	"__is_integral",
	"__is_floating_point",
	"__is_arithmetic",
	"__is_signed",
	"__is_unsigned",
	"__is_array",
	"__is_pointer",
	"__is_reference",
	"__is_lvalue_reference",
	"__is_rvalue_reference",
	"__is_function",
	"__is_member_pointer",
	"__is_member_object_pointer",
	"__is_member_function_pointer",
	"__is_enum",
	"__is_union",
	"__is_class",
	"__is_scalar",
	"__is_empty",
	"__is_final",
	"__is_polymorphic",
	"__is_abstract",
	"__is_base_of",
	"__has_virtual_destructor",
	// would-it-compile probes (sema/sem_trait.cpp EvaluateSemaProbeTrait)
	"__is_constructible",
	"__is_nothrow_constructible",
	"__is_trivially_constructible",
	"__is_convertible",
	"__is_convertible_to",
	"__is_assignable",
	"__is_nothrow_assignable",
	"__is_trivially_assignable",
	"__is_destructible",
	"__is_nothrow_destructible",
	"__is_trivially_destructible",
	"__has_trivial_constructor",
	"__is_pod",
	"__is_trivial",
	"__is_trivially_copyable",
	"__is_standard_layout",
	"__is_literal_type",
	"__array_rank",
	"__reference_constructs_from_temporary",
	"__reference_binds_to_temporary",
	0
};

const char * const kSupportedAttributes[] = {
	// using_if_exists: a using-declaration whose target may be missing
	// binds nothing instead of erroring (hosted libc++-style headers).
	"using_if_exists",
	0
};

set<string> MakeNameSet(const char * const * names)
{
	set<string> result;
	for (; *names; names++)
		result.insert(*names);
	return result;
}

}  // namespace

bool HostedProbeHasBuiltin(const string & name)
{
	static const set<string> builtins = MakeNameSet(kSupportedBuiltins);
	return builtins.count(name) != 0 || HostedBuiltinTransformName(name) ||
		HostedBuiltinTraitName(name) ||
		!LibcBuiltinSignature(name).empty();
}

bool HostedBuiltinTransformName(const string & name)
{
	static const set<string> transforms = MakeNameSet(kBuiltinTransforms);
	return transforms.count(name) != 0;
}

bool HostedBuiltinTraitName(const string & name)
{
	static const set<string> traits = MakeNameSet(kBuiltinTraits);
	return traits.count(name) != 0;
}

bool HostedProbeHasFeature(const string & name)
{
	static const set<string> features = MakeNameSet(kSupportedFeatures);
	const string stripped = StripUnderscoreDecoration(name);
	if (features.count(stripped) != 0)
		return true;
	// Trait-primitive availability queries (`__has_extension(is_pod)`)
	// answer from the trait registry, so they track implementation.
	if (stripped.compare(0, 3, "is_") == 0 ||
	    stripped.compare(0, 4, "has_") == 0)
		return HostedBuiltinTraitName("__" + stripped);
	return false;
}

bool HostedProbeHasAttribute(const string & name)
{
	static const set<string> attributes = MakeNameSet(kSupportedAttributes);
	return attributes.count(StripUnderscoreDecoration(name)) != 0;
}

bool HostedProbeHasCppAttribute(const string & name)
{
	(void)name;
	return false;
}

namespace {

// The three-variant C math families ("" double / f float / l long
// double); r/R in the pattern resolve to the variant's real type.
struct MathBuiltinFamily
{
	const char * name;
	const char * pattern;
};

const MathBuiltinFamily kMathFamilies[] = {
	{"acos", "rr"}, {"acosh", "rr"}, {"asin", "rr"}, {"asinh", "rr"},
	{"atan", "rr"}, {"atanh", "rr"}, {"cbrt", "rr"}, {"ceil", "rr"},
	{"cos", "rr"}, {"cosh", "rr"}, {"erf", "rr"}, {"erfc", "rr"},
	{"exp", "rr"}, {"exp2", "rr"}, {"expm1", "rr"}, {"fabs", "rr"},
	{"floor", "rr"}, {"lgamma", "rr"}, {"log", "rr"}, {"log10", "rr"},
	{"log1p", "rr"}, {"log2", "rr"}, {"logb", "rr"},
	{"nearbyint", "rr"}, {"rint", "rr"}, {"round", "rr"},
	{"sin", "rr"}, {"sinh", "rr"}, {"sqrt", "rr"}, {"tan", "rr"},
	{"tanh", "rr"}, {"tgamma", "rr"}, {"trunc", "rr"},
	{"atan2", "rrr"}, {"copysign", "rrr"}, {"fdim", "rrr"},
	{"fmax", "rrr"}, {"fmin", "rrr"}, {"fmod", "rrr"},
	{"hypot", "rrr"}, {"nextafter", "rrr"}, {"pow", "rrr"},
	{"remainder", "rrr"},
	{"ldexp", "rri"}, {"scalbn", "rri"}, {"scalbln", "rrl"},
	{"nexttoward", "rre"},
	{"ilogb", "ir"}, {"lrint", "lr"}, {"lround", "lr"},
	{"llrint", "xr"}, {"llround", "xr"},
	{"fma", "rrrr"}, {"frexp", "rrI"}, {"modf", "rrR"},
	{"remquo", "rrrI"},
	{0, 0}
};

// Fixed-signature libc-backed builtins (string/memory/utility).
const MathBuiltinFamily kLibcSingles[] = {
	{"strlen", "ms"}, {"memcpy", "ppqm"}, {"memmove", "ppqm"},
	{"strcmp", "iss"}, {"memcmp", "iqqm"}, {"memchr", "pqim"},
	{"strchr", "csi"}, {"bzero", "vpm"}, {"free", "vp"},
	{"abs", "ii"}, {"labs", "ll"}, {"llabs", "xx"},
	{"fabsf128", "ee"},
	{0, 0}
};

// Resolves r/R in a family pattern for one variant suffix.
string ResolveMathPattern(const string & pattern, char real_code)
{
	string resolved;
	for (size_t i = 0; i < pattern.size(); i++)
	{
		if (pattern[i] == 'r')
			resolved += real_code;
		else if (pattern[i] == 'R')
		{
			resolved += 'P';
			resolved += real_code;
		}
		else
			resolved += pattern[i];
	}
	return resolved;
}

}  // namespace

string LibcBuiltinSignature(const string & name)
{
	if (name.compare(0, 10, "__builtin_") != 0)
		return string();
	string tail = name.substr(10);
	for (const MathBuiltinFamily * at = kLibcSingles; at->name; at++)
		if (tail == at->name)
			return at->pattern;
	for (const MathBuiltinFamily * at = kMathFamilies; at->name; at++)
	{
		string base = at->name;
		if (tail == base)
			return ResolveMathPattern(at->pattern, 'd');
		if (tail == base + "f")
			return ResolveMathPattern(at->pattern, 'f');
		if (tail == base + "l")
			return ResolveMathPattern(at->pattern, 'e');
	}
	return string();
}

string HostLibraryBuiltinSymbol(const string & name)
{
	if (LibcBuiltinSignature(name).empty())
		return string();
	return name.substr(10);  // strlen("__builtin_")
}
