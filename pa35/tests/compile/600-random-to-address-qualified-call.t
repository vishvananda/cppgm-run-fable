#include <random>
#include <type_traits>
static_assert(std::mt19937::default_seed == 5489u, "<random> mt19937 default_seed");
int to_address_anchor(int * ptr)
{
  return *std::__to_address(ptr);
}
static_assert(sizeof(&to_address_anchor) > 0, "std::__to_address body anchor");
