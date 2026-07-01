// VALIDATION: compile-pass
// A defaulted enable-if guard may name a static constexpr member template with
// an unnamed non-type pack.

template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct check {
  template<int&...>
  static constexpr bool ok()
  {
    return true;
  }
};

template<class T>
struct Box {
  template<class Check = check<T>, enable_if_t<Check::ok(), int> = 0>
  Box() {}
};

int main()
{
  Box<int> b;
  (void)b;
  return 0;
}
