template<bool B>
struct bool_ {
  static const bool value = B;
};

typedef bool_<true> true_;
typedef bool_<false> false_;

template<bool C, class T, class E>
struct if_c {
  typedef T type;
};

template<class T, class E>
struct if_c<false, T, E> {
  typedef E type;
};

template<class C, class T, class E>
using if_ = typename if_c<static_cast<bool>(C::value), T, E>::type;

template<class T>
struct identity {
  typedef T type;
};

template<template<class...> class F, class... T>
struct valid_impl {
  template<template<class...> class G, class = G<T...> >
  static true_ check(int);

  template<template<class...> class>
  static false_ check(...);

  typedef decltype(check<F>(0)) type;
};

template<template<class...> class F, class... T>
using valid = typename valid_impl<F, T...>::type;

template<template<class...> class F, class... T>
struct defer_impl {
  typedef F<T...> type;
};

struct no_type {};

template<template<class...> class F, class... T>
using defer = if_<valid<F, T...>, defer_impl<F, T...>, no_type>;

template<template<class...> class F>
struct quote_trait {
  template<class... T>
  using fn = typename F<T...>::type;
};

typedef quote_trait<identity> quoted_identity;
typedef valid<quoted_identity::template fn, void> result;

int main() {
  return result::value ? 0 : 1;
}
