#include <string>
#include <utility>
#include <type_traits>
static_assert(std::is_move_constructible<std::string>::value, "std::string move-constructible");
struct Holder { Holder(std::string s) : value(std::move(s)) {} std::string value; };
Holder holder_anchor()
{
  return Holder((std::string()));
}
static_assert(sizeof(&holder_anchor) > 0, "string by-value body anchor");
