#pragma once

#include <map>
#include <string>
#include <vector>

using std::map;
using std::string;
using std::vector;

#include "pp_token.h"

// The one spelling of the variadic-arguments identifier (16.3p5).
extern const char* const kMacroVaArgs;

// Builtin (dynamically valued) predefined macros: the expander asks its
// IBuiltinTokenSource for the produced token instead of substituting a
// stored replacement list.
enum EMacroBuiltin
{
	kBuiltinNone,
	kBuiltinFile,
	kBuiltinLine,
	// PA34: a hosted probe operator name (__has_include, __has_builtin,
	// ...). Defined for #ifdef/defined; controlling expressions fold its
	// invocations before expansion, and a stray text occurrence produces
	// itself (inert, the invoking name is painted).
	kBuiltinHostedProbe
};

// One macro definition (16.3). The replacement list is stored trimmed
// (no leading/trailing whitespace flags beyond the first token, whose
// ws_before is not part of the 16.3p1 identity) with paste_op set on the
// ## tokens that operate during substitution and param_index stamped on
// every token, so substitution never re-derives parameter slots from
// spellings.
struct MacroDefinition
{
	MacroDefinition()
		: function_like(false), variadic(false), has_paste(false),
		  builtin(kBuiltinNone)
	{}

	string name;
	bool function_like;
	bool variadic;
	bool has_paste;
	vector<string> params;
	vector<PPToken> replacement;
	EMacroBuiltin builtin;
};

// The set of macros defined at the current point of the file. Define and
// Undef take the directive's tokens after the `define`/`undef` keyword,
// up to but not including the terminating new-line, and throw
// std::runtime_error for every definition-time constraint violation
// (16.3p1-p6, 16.3.2p1, 16.3.3p1, __VA_ARGS__ placement).
class MacroTable
{
public:
	void Define(const vector<PPToken>& line);
	void Undef(const vector<PPToken>& line);

	// Registers an object-like predefined macro whose invocation produces
	// a dynamically computed token (__FILE__/__LINE__).
	void DefineBuiltin(const string& name, EMacroBuiltin builtin);

	// Null when no macro of that name is defined.
	const MacroDefinition* Lookup(const string& name) const;

private:
	map<string, MacroDefinition> macros_;
};
