// Hosted builtin-trait compatibility: __is_constructible(T, U...) must treat
// function reference targets as references, not as unconstructible function
// object types, while substituting defaulted SFINAE parameters.
struct F {};

F &get_f();
int &get_i();

template<bool B>
struct bool_constant {
  static const bool value = B;
};

template<class...>
struct and_;

template<>
struct and_<> : bool_constant<true> {};

template<class T>
struct and_<T> : bool_constant<T::value> {};

template<class T, class... Rest>
struct and_<T, Rest...> : bool_constant<T::value && and_<Rest...>::value> {};

template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<class T, class... Args>
struct is_constructible : bool_constant<__is_constructible(T, Args...)> {};

template<class... T>
struct tuple {
  template<class, class... U>
  struct enable_ctor_impl : bool_constant<false> {};

  template<class... U>
  struct enable_ctor_impl<bool_constant<true>, U...> : and_<is_constructible<T, U>...> {};

  template<class... U>
  struct enable_ctor
      : enable_ctor_impl<bool_constant<(sizeof...(U) == sizeof...(T))>, U...> {};

  template<class... U, typename enable_if<enable_ctor<U...>::value, int>::type = 0>
  explicit tuple(U &&...) {}
};

template<class... A>
struct rrlist {
  typedef tuple<A &...> data_type;
  data_type data;

  explicit rrlist(A &... a) : data(a...) {}
};

template<class... A>
int call(A &&... a) {
  rrlist<A...> r(a...);
  (void)r;
  return 0;
}

int main() {
  return call(get_f, get_i);
}
