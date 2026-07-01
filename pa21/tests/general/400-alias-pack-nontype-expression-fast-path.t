// Reduced from Boost.MP11 mp_count_if. A member alias selected through a
// partial specialization must evaluate a non-type template argument whose
// expression expands a type pack and applies a bound template-template
// parameter inside the expansion.
typedef unsigned long size_t;

template<class T, T V>
struct constant {
  static const T value = V;
};

template<bool B>
using bool_ = constant<bool, B>;

template<size_t N>
using size_ = constant<size_t, N>;

template<class T>
using mp_to_bool = bool_<T::value != 0>;

template<class... T>
struct list {
};

constexpr size_t cx_plus()
{
  return 0;
}

template<class T1, class... T>
constexpr size_t cx_plus(T1 t1, T... t)
{
  return static_cast<size_t>(t1) + cx_plus(t...);
}

template<class L, template<class...> class P>
struct count_if_impl;

template<template<class...> class L, class... T, template<class...> class P>
struct count_if_impl<L<T...>, P> {
  using type = size_<cx_plus(mp_to_bool<P<T> >::value...)>;
};

template<class L, template<class...> class P>
using count_if = typename count_if_impl<L, P>::type;

struct X {
};

template<class T>
struct pred {
  static const bool value = false;
};

using L = list<X, X>;
using R = count_if<L, pred>;

int main()
{
  return R::value;
}
