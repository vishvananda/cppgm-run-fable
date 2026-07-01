// VALIDATION: compile-pass
// Boost.MP11-style validity probing must keep resolved alias-template
// arguments canonical when expanding a nested alias target. Reusing the
// original source spelling can re-evaluate mp_valid<F, T...> after T has
// been rebound by the nested alias.

template<class T, T V>
struct integral_constant {
  static const T value = V;
};

template<class T, class U>
struct is_same : integral_constant<bool, false> {};

template<class T>
struct is_same<T, T> : integral_constant<bool, true> {};

template<bool B>
using bool_constant = integral_constant<bool, B>;

using true_type = bool_constant<true>;
using false_type = bool_constant<false>;

template<unsigned long N>
using size_constant = integral_constant<unsigned long, N>;

constexpr unsigned long count_true()
{
  return 0;
}

template<class T1, class... T>
constexpr unsigned long count_true(T1 t1, T... rest)
{
  return static_cast<unsigned long>(t1) + count_true(rest...);
}

template<class... T>
struct list {};

template<bool C, class T, class... E>
struct if_c_impl {};

template<class T, class... E>
struct if_c_impl<true, T, E...> {
  using type = T;
};

template<class T, class E>
struct if_c_impl<false, T, E> {
  using type = E;
};

template<class C, class T, class... E>
using if_ = typename if_c_impl<static_cast<bool>(C::value), T, E...>::type;

template<class L, class V>
struct count_impl;

template<template<class...> class L, class... T, class V>
struct count_impl<L<T...>, V> {
  using type = size_constant<count_true(is_same<T, V>::value...)>;
};

template<class L, class V>
using count = typename count_impl<L, V>::type;

template<class T>
using to_bool = bool_constant<static_cast<bool>(T::value)>;

template<class... T>
using any = bool_constant<count<list<to_bool<T>...>, true_type>::value != 0>;

template<template<class...> class F, class... T>
struct valid_impl {
  template<template<class...> class G, class = G<T...>>
  static true_type check(int);

  template<template<class...> class>
  static false_type check(...);

  using type = decltype(check<F>(0));
};

template<template<class...> class F, class... T>
using valid = typename valid_impl<F, T...>::type;

template<template<class...> class F, class... T>
struct defer_impl {
  using type = F<T...>;
};

struct no_type {};

template<template<class...> class F, class... T>
using defer = if_<valid<F, T...>, defer_impl<F, T...>, no_type>;

using D = defer<any, false_type>;
using R = typename D::type;

int main()
{
  return R::value ? 1 : 0;
}
