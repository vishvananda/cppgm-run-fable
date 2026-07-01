#include <functional>
#include <type_traits>
static_assert(std::is_same<std::function<int()>::result_type, int>::value, "std::function<int()>::result_type");
int one() { return 1; }
void function_object_anchor()
{
  std::function<int()> g = one;
  (void)g;
}
static_assert(sizeof(&function_object_anchor) > 0, "std::function body anchor");
