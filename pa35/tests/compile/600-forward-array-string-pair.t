#include <string>
#include <utility>

enum E
{
  A
};

template<class T>
std::pair<const std::string, E> make_pairish(T&& t)
{
  std::string local(std::forward<T>(t));
  return std::pair<const std::string, E>(std::forward<T>(t), A);
}

std::pair<const std::string, E> pairish_anchor()
{
  return make_pairish("(");
}
static_assert(sizeof(&pairish_anchor) > 0, "forwarded pair construction anchor");
