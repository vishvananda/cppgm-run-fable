#include <functional>
#include <type_traits>
static_assert(std::is_same<std::reference_wrapper<int>::type, int>::value, "reference_wrapper<int>::type");
