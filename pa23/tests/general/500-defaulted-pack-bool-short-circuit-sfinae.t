// VALIDATION: compile-pass
// A defaulted bool template parameter that short-circuits from a pack-size
// comparison must become concrete before the following enable-if aliases are
// substituted.

template<bool B, typename T = bool>
struct enable_if {};

template<typename T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, typename T = bool>
using enable_if_t = typename enable_if<B, T>::type;

template<bool B>
struct gate {
  template<typename...>
  static constexpr bool yes()
  {
    return B;
  }

  template<typename...>
  static constexpr bool no()
  {
    return !B;
  }
};

template<typename... Elements>
struct box {
  template<typename>
  static constexpr bool other()
  {
    return false;
  }

  template<bool B, typename... Args>
  using yes_t = enable_if_t<gate<B>::template yes<Args...>(), bool>;

  template<bool B, typename... Args>
  using no_t = enable_if_t<gate<B>::template no<Args...>(), bool>;

  template<typename... Args,
           bool Valid = (sizeof...(Elements) == sizeof...(Args)) &&
                        !other<const box<Args...> &>(),
           yes_t<Valid, const Args&...> = true>
  box(const box<Args...> &)
  {}

  template<typename... Args,
           bool Valid = (sizeof...(Elements) == sizeof...(Args)) &&
                        !other<const box<Args...> &>(),
           no_t<Valid, const Args&...> = false>
  box(const box<Args...> &)
  {}
};

void test(const box<int, int> & source)
{
  box<const box<int, int> &> value(source);
  (void)value;
}

int main()
{
  return 0;
}
