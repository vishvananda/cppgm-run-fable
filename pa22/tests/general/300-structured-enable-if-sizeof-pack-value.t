// VALIDATION: compile-pass
// A defaulted non-type SFINAE guard may depend on sizeof... of the class pack
// after substitution.

template<bool B>
struct bool_constant {
  static const bool value = B;
};

template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class...>
struct all : bool_constant<true> {};

template<class... T>
struct tuple {
  template<class Dummy = void,
           enable_if_t<all<bool_constant<sizeof...(T) >= 1> >::value, int> = 0>
  tuple() {}
};

int main()
{
  tuple<int> value;
  (void)value;
  return 0;
}
