static_assert(__is_nothrow_assignable(int&, int), "");

template<class T>
inline constexpr bool can_nothrow_assign_v = __is_nothrow_assignable(T&, T const&);

static_assert(can_nothrow_assign_v<unsigned long>, "");

int main() {
  return 0;
}
