#pragma once

#include <string>
#include <utility>
#include <vector>

using std::pair;
using std::string;
using std::vector;

// The course-defined predefined object-like macros with fixed
// replacement lists, shared by every tool that embeds the PA5
// preprocessor; __FILE__/__LINE__ are installed by the Preprocessor as
// builtins. __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss") slice
// the asctime format "Www Mmm dd hh:mm:ss yyyy\n", so tools call this
// once at entry and share the result across srcfiles.
vector<pair<string, string>> PredefinedObjectMacros();
