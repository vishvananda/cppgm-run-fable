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
	"__builtin_unreachable",
	"__builtin_isnan",
	"__builtin_nanl",
	"__builtin_va_start",
	"__builtin_va_end",
	"__builtin_alloca",
	"__builtin_bswap16",
	"__builtin_bswap32",
	"__builtin_bswap64",
	"__builtin_popcountg",
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
	return builtins.count(name) != 0;
}

bool HostedProbeHasFeature(const string & name)
{
	static const set<string> features = MakeNameSet(kSupportedFeatures);
	return features.count(StripUnderscoreDecoration(name)) != 0;
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
