#pragma once

#include <string>
#include <vector>

// PA34 hosted environment: the build-time host-compiler probe (predefined
// macros and standard include paths, captured by gen_builtin_host_config.pl
// into the generated cppgm_builtin_host_config.h) plus the harness's
// CPPGM_STDINC_PATHS runtime injection point. Only the cppgm++ driver and
// its toolchain layer link this module; the PA5 preproc tool keeps the
// unhosted environment.

class Preprocessor;

namespace toolchain {

// The host compiler's `-dM -E` predefined macro set as directive bodies
// (the text after `#define `, e.g. "__GNUC__ 15" or "__INT8_C(c) c"),
// in probe order. Names owned by the course predefined set (__cplusplus,
// __DATE__, __TIME__, __FILE__, __LINE__, __STDC_HOSTED__, __CPPGM__,
// __CPPGM_AUTHOR__) are filtered out here so the course values stay
// authoritative.
const std::vector<std::string> & HostPredefinedMacroBodies();

// The baked host standard include paths from the build-time probe.
std::vector<std::string> HostStandardIncludeDirs();

// The probed host compiler spelling and version banner data for the
// direct driver query flags.
std::string HostCompilerSpelling();

// One driver preprocessor control, in command order: 'D' (define spec
// "name[=value]"), 'U' (undef name), 'i' (-include file).
struct HostedDriverAction
{
	HostedDriverAction() : kind(0) {}
	HostedDriverAction(char k, const std::string & v) : kind(k), value(v) {}

	char kind;
	std::string value;
};

struct HostedPreprocessConfig
{
	std::vector<HostedDriverAction> actions;
	std::vector<std::string> isystem_dirs;
	// Extra standard include directories the driver was configured with
	// (the harness's CPPGM_STDINC_PATHS entries), searched between the
	// -isystem dirs and the baked host standard paths.
	std::vector<std::string> stdinc_dirs;
};

// Applies the hosted environment to one Preprocessor: hosted mode, the
// host predefined macro import, the -D/-U/-include actions in command
// order, and the system include chain (-isystem dirs first, then the
// hosted standard paths).
void ConfigureHostedPreprocessor(::Preprocessor & preprocessor,
                                 const HostedPreprocessConfig & config);

}  // namespace toolchain
