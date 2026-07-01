struct true_type {
  static const bool value = true;
};

struct false_type {
  static const bool value = false;
};

template<class...>
struct voider {
  using type = void;
};

template<class... T>
using void_t = typename voider<T...>::type;

template<template<class...> class F, class... T>
struct valid_impl {
  template<template<class...> class G, class = G<T...> >
  static true_type check(int);

  template<template<class...> class>
  static false_type check(...);

  using type = decltype(check<F>(0));
};

template<template<class...> class F, class... T>
using valid = typename valid_impl<F, T...>::type;

template<class A, class B>
using exactly_two = void_t<A, B>;

template<class... T>
struct outer {
  template<template<class...> class F, class... U>
  using inner_valid = valid<F, U...>;

  using type = inner_valid<exactly_two, int, long>;
};

static_assert(outer<char, short, float>::type::value,
              "inner alias pack must not expand the outer pack");

int main()
{
  return outer<char, short, float>::type::value ? 0 : 1;
}
