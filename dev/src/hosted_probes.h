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

// __has_feature / __has_extension. Leading/trailing double underscores
// on the queried name are stripped first (clang spelling equivalence).
// Every supported feature is also a supported extension.
bool HostedProbeHasFeature(const std::string & name);

// __has_attribute: GNU attribute names the compiler implements (not
// merely parses and discards). Underscore-delimited spellings match.
bool HostedProbeHasAttribute(const std::string & name);

// __has_cpp_attribute: C++ standard attribute probe.
bool HostedProbeHasCppAttribute(const std::string & name);
