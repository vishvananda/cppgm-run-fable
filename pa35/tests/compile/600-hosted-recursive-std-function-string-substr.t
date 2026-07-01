#include <functional>
#include <string>
#include <type_traits>

static_assert(std::is_same<std::function<bool(const std::string&, const std::string&)>::result_type, bool>::value,
              "std::function<...>::result_type");

bool recursive_function_anchor()
{
  std::function<bool(const std::string &, const std::string &)> f;
  f = [&](const std::string & a, const std::string & b)
  {
    if(a.empty() || b.empty()) { return true; }
    return f(a.substr(1), b.substr(1));
  };
  return f("abc", "abc");
}
static_assert(sizeof(&recursive_function_anchor) > 0, "recursive std::function body anchor");
