#include <tuple>
#include <type_traits>
static_assert(std::is_same<decltype(std::forward_as_tuple(7)), std::tuple<int&&> >::value, "forward_as_tuple(rvalue) -> tuple<int&&>");
struct S { int v; };
S * forward_as_tuple_anchor()
{
  S s; s.v = 7;
  auto t = std::forward_as_tuple(static_cast<S &&>(s));
  return &std::get<0>(t);
}
static_assert(sizeof(&forward_as_tuple_anchor) > 0, "forward_as_tuple body anchor");
