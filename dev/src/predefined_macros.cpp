#include "predefined_macros.h"

#include <ctime>

using std::make_pair;

vector<pair<string, string>> PredefinedObjectMacros()
{
	time_t now = time(0);
	string stamp = asctime(localtime(&now));
	vector<pair<string, string>> macros;
	macros.push_back(make_pair("__CPPGM__", "201303L"));
	macros.push_back(make_pair("__cplusplus", "201103L"));
	macros.push_back(make_pair("__STDC_HOSTED__", "1"));
	macros.push_back(make_pair("__CPPGM_AUTHOR__",
	                           "\"Vishvananda Abrams\""));
	macros.push_back(make_pair("__DATE__", "\"" + stamp.substr(4, 7) +
	                                       stamp.substr(20, 4) + "\""));
	macros.push_back(make_pair("__TIME__",
	                           "\"" + stamp.substr(11, 8) + "\""));
	return macros;
}
