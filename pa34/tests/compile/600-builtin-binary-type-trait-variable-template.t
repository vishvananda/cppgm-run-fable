template<class T>
inline constexpr bool is_void_v = __is_same(__remove_cv(T), void);

template<class T>
inline constexpr bool can_assign_v = __is_assignable(T&, T const&);

template<class From, class To>
inline constexpr bool convertible_v = __is_convertible(From, To);

static_assert(is_void_v<void const>, "");
static_assert(can_assign_v<unsigned long>, "");
static_assert(convertible_v<int, long>, "");

int main() {
  return 0;
}
