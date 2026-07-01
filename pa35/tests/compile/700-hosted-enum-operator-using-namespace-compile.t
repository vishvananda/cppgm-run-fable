#include <algorithm>
#include <string>
#include <type_traits>
static_assert(std::is_same<std::string::value_type, char>::value, "<string> value_type");
using namespace std;
enum class HeaderState { Hash, Other };
bool is_include(HeaderState state, const string & text)
{ return state == HeaderState::Hash && text == "include"; }
static_assert(sizeof(&is_include) > 0, "enum comparison body anchor");
