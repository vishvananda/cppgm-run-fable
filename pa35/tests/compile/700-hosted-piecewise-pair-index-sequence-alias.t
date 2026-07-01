#include <string>
#include <tuple>
#include <utility>
#include <type_traits>
struct V {};
static_assert(std::tuple_size<std::tuple<std::string const &> >::value == 1, "tuple_size");
static_assert(std::is_same<std::pair<std::string const, V>::first_type, std::string const>::value, "pair first_type");
std::pair<std::string const, V> f(std::string const &s, V const &v) {
  std::tuple<std::string const &> first(s);
  std::tuple<V const &> second(v);
  return std::pair<std::string const, V>(std::piecewise_construct, first, second);
}
static_assert(sizeof(&f) > 0, "piecewise pair body anchor");
