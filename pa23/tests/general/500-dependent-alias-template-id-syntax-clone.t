template<bool V>
struct bool_constant {
  static const bool value = V;
};

typedef bool_constant<true> true_type;
typedef bool_constant<false> false_type;

template<bool C, class T, class E>
struct if_c_impl {
};

template<class T, class E>
struct if_c_impl<true, T, E> {
  typedef T type;
};

template<class T, class E>
struct if_c_impl<false, T, E> {
  typedef E type;
};

template<class C, class T, class E>
using if_ = typename if_c_impl<static_cast<bool>(C::value), T, E>::type;

template<template<class...> class F, class... T>
struct valid_impl {
  template<template<class...> class G, class = G<T...> >
  static true_type check(int);

  template<template<class...> class>
  static false_type check(...);

  typedef decltype(check<F>(0)) type;
};

template<template<class...> class F, class... T>
using valid = typename valid_impl<F, T...>::type;

template<template<class...> class F, class... T>
struct defer_impl {
  typedef F<T...> type;
};

struct no_type {
};

template<template<class...> class F, class... T>
using defer = if_<valid<F, T...>, defer_impl<F, T...>, no_type>;

template<bool C, class T, template<class...> class F, class... U>
struct eval_if_impl;

template<class T, template<class...> class F, class... U>
struct eval_if_impl<true, T, F, U...> {
  typedef T type;
};

template<class T, template<class...> class F, class... U>
struct eval_if_impl<false, T, F, U...> : defer<F, U...> {
};

template<class C, class T, template<class...> class F, class... U>
using eval_if =
    typename eval_if_impl<static_cast<bool>(C::value), T, F, U...>::type;

template<class C, class T, class... E>
struct cond_impl;

template<class C, class T, class... E>
using cond = typename cond_impl<C, T, E...>::type;

template<class C, class T, class... E>
using cond_ = eval_if<C, T, cond, E...>;

template<class C, class T, class... E>
struct cond_impl : defer<cond_, C, T, E...> {
};

struct selected {
};

struct skipped {
};

static_assert(valid<cond_,
                    false_type,
                    skipped,
                    true_type,
                    selected,
                    true_type,
                    void>::value,
              "alias template-id syntax must be cloned per substitution");

int main()
{
  return valid<cond_,
               false_type,
               skipped,
               true_type,
               selected,
               true_type,
               void>::value ? 0 : 1;
}
