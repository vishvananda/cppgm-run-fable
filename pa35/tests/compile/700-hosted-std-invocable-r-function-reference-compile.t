#include <type_traits>
void accept(unsigned long) {}
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 210000
static_assert(std::__is_invocable_r_v<void, decltype(accept)&, unsigned long>, "callable");
#endif
static_assert(sizeof(&accept) > 0, "function reference body anchor");
