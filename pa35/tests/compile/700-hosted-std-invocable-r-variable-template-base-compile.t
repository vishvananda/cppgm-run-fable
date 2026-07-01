#include <type_traits>
#include <string>

struct MakeString {
  std::string operator()();
};

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 210000
static_assert(std::__is_invocable_r_v<std::string, MakeString&>, "callable");
#endif
static_assert(sizeof(MakeString) > 0, "callable object anchor");
