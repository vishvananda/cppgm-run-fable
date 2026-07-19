#pragma once

#include <string>

// PA34 capability registry: the single source of truth for what this
// compiler implements, answering the hosted preprocessor probe operators
// (__has_builtin, __has_feature, __has_extension, __has_attribute,
// __has_cpp_attribute). The sema builtin-identifier lookups grow in step
// with these tables so probe answers never drift from what the compiler
// actually accepts. Answers are honest: a builtin is listed only once the
// compiler implements it.

// __has_builtin: builtin functions, builtin type traits, and builtin
// type transforms referenced by their reserved identifiers.
bool HostedProbeHasBuiltin(const std::string & name);

// PA34 C-library-backed builtin registry: `__builtin_X` forms that
// call the host C library function X. Returns the compact signature
// pattern for a full builtin name ("" when the name is not
// libc-backed). Pattern codes (first character is the result):
// v void, b bool, i int, l long, x long long, m unsigned long,
// p void*, q const void*, s const char*, c char*, f float, d double,
// e long double, I int*, and for the three-variant math families the
// family's own real type r / pointer-to-real R (resolved per
// variant: "" double, f float, l long double).
std::string LibcBuiltinSignature(const std::string & name);

// The host C library symbol a registered `__builtin_X` calls under
// separate compilation ("" when the name is not libc-backed).
std::string HostLibraryBuiltinSymbol(const std::string & name);

// __has_feature / __has_extension. Leading/trailing double underscores
// on the queried name are stripped first (clang spelling equivalence).
// Every supported feature is also a supported extension.
bool HostedProbeHasFeature(const std::string & name);

// __has_attribute: GNU attribute names the compiler implements (not
// merely parses and discards). Underscore-delimited spellings match.
bool HostedProbeHasAttribute(const std::string & name);

// __has_cpp_attribute: C++ standard attribute probe.
bool HostedProbeHasCppAttribute(const std::string & name);

// Builtin type-transform identifiers (`__remove_cv(T)` and family):
// parsed as type specifiers, evaluated by the sema type builder. The
// __has_builtin answer for these names follows this same table.
bool HostedBuiltinTransformName(const std::string & name);

// Builtin type-trait identifiers (`__is_same(T, U)` and family):
// parsed as EK_BUILTIN_TRAIT expressions, evaluated by the sema to
// bool constants. The __has_builtin answer follows this same table.
bool HostedBuiltinTraitName(const std::string & name);
