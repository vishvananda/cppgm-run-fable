#include <tuple>
#include <type_traits>
static_assert(std::is_same<std::tuple_element<0, std::tuple<int> >::type, int>::value, "tuple_element<0>");
namespace test_case {
template<class Tuple, unsigned long I>
int first_value(Tuple& t) { return get<I>(t); }
}
int tuple_get_anchor()
{
  std::tuple<int> t(7);
  return test_case::first_value<std::tuple<int>, 0>(t);
}
static_assert(sizeof(&tuple_get_anchor) > 0, "tuple get ADL body anchor");
