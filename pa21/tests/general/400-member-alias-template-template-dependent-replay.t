template<class T>
struct is_volatile {
  static const bool value = false;
};

template<class T>
struct is_volatile<T volatile> {
  static const bool value = true;
};

template<bool C, class T, class F>
struct if_c {
  typedef T type;
};

template<class T, class F>
struct if_c<false, T, F> {
  typedef F type;
};

template<class C, class T, class F>
using if_ = typename if_c<C::value, T, F>::type;

template<class...>
struct list {};

template<template<class...> class F, class... T>
struct defer {
  typedef F<T...> type;
};

template<template<class...> class F>
struct quote {
  template<class... T>
  using fn = typename defer<F, T...>::type;
};

template<class L, template<class...> class P, class W>
struct replace_if_impl;

template<template<class...> class L,
         class... T,
         template<class...> class P,
         class W>
struct replace_if_impl<L<T...>, P, W> {
  template<class U>
  using f = if_<P<U>, W, U>;

  typedef L<f<T>...> type;
};

template<class L, class Q, class W>
using replace_if_q = typename replace_if_impl<L, Q::template fn, W>::type;

template<class, class>
struct same {
  static const bool value = false;
};

template<class T>
struct same<T, T> {
  static const bool value = true;
};

struct X {};

typedef replace_if_q<list<X, X volatile>, quote<is_volatile>, void> R;
static_assert(same<R, list<X, void> >::value,
              "member alias template argument should resolve after substitution");

int main() {
  return 0;
}
