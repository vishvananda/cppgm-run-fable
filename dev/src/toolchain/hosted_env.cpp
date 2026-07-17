#include "toolchain/hosted_env.h"

#include "cppgm_builtin_host_config.h"
#include "preprocess.h"

using std::string;
using std::vector;

namespace toolchain {

namespace {

bool IsOwnedPredefinedName(const string & name)
{
	return name == "__cplusplus" ||
		name == "__DATE__" ||
		name == "__TIME__" ||
		name == "__FILE__" ||
		name == "__LINE__" ||
		name == "__STDC_HOSTED__" ||
		name == "__CPPGM__" ||
		name == "__CPPGM_AUTHOR__";
}

// The name of one `#define <name>[(params)] <replacement>` probe line:
// the identifier characters after "#define ".
string DefineLineName(const string & body)
{
	size_t end = 0;
	while (end < body.size() &&
	       (body[end] == '_' ||
	        (body[end] >= 'a' && body[end] <= 'z') ||
	        (body[end] >= 'A' && body[end] <= 'Z') ||
	        (end > 0 && body[end] >= '0' && body[end] <= '9')))
		end++;
	return body.substr(0, end);
}

vector<string> ParseHostPredefinedMacroBodies()
{
	const string text = cppgm_builtin_host_config::kHostPredefinedMacros;
	const string prefix = "#define ";
	vector<string> bodies;
	size_t pos = 0;
	while (pos < text.size())
	{
		size_t eol = text.find('\n', pos);
		if (eol == string::npos)
			eol = text.size();
		string line = text.substr(pos, eol - pos);
		pos = eol + 1;
		if (line.compare(0, prefix.size(), prefix) != 0)
			continue;
		string body = line.substr(prefix.size());
		if (body.empty() || IsOwnedPredefinedName(DefineLineName(body)))
			continue;
		bodies.push_back(body);
	}
	return bodies;
}

}  // namespace

const vector<string> & HostPredefinedMacroBodies()
{
	static const vector<string> bodies = ParseHostPredefinedMacroBodies();
	return bodies;
}

vector<string> HostStandardIncludeDirs()
{
	vector<string> dirs;
	for (const char * const * path =
	         cppgm_builtin_host_config::kStandardIncludePaths;
	     *path; path++)
		dirs.push_back(*path);
	return dirs;
}

string HostCompilerSpelling()
{
	return cppgm_builtin_host_config::kHostCxx;
}

void ConfigureHostedPreprocessor(::Preprocessor & preprocessor,
                                 const HostedPreprocessConfig & config)
{
	preprocessor.EnableHostedMode();
	const vector<string> & host_macros = HostPredefinedMacroBodies();
	for (size_t i = 0; i < host_macros.size(); i++)
		preprocessor.DefineHostMacroLine(host_macros[i]);
	for (size_t i = 0; i < config.actions.size(); i++)
	{
		const HostedDriverAction & action = config.actions[i];
		if (action.kind == 'D')
			preprocessor.DefineCommandLineMacro(action.value);
		else if (action.kind == 'U')
			preprocessor.UndefCommandLineMacro(action.value);
		else if (action.kind == 'i')
			preprocessor.AddPreIncludeFile(action.value);
	}
	for (size_t i = 0; i < config.isystem_dirs.size(); i++)
		preprocessor.AddSystemIncludeDir(config.isystem_dirs[i]);
	for (size_t i = 0; i < config.stdinc_dirs.size(); i++)
		preprocessor.AddSystemIncludeDir(config.stdinc_dirs[i]);
	vector<string> host_dirs = HostStandardIncludeDirs();
	for (size_t i = 0; i < host_dirs.size(); i++)
		preprocessor.AddSystemIncludeDir(host_dirs[i]);
}

}  // namespace toolchain
