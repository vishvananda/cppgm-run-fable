typedef unsigned long size_t;

template<class T, T V>
struct integral_constant {
  static const T value = V;
};

template<class... T>
struct list {};

template<class L>
struct size_impl;

template<template<class...> class L, class... T>
struct size_impl<L<T...> > {
  typedef integral_constant<size_t, sizeof...(T)> type;
};

template<class L>
using size = typename size_impl<L>::type;

template<bool B>
using bool_ = integral_constant<bool, B>;

template<class T>
struct gate {
  static const bool value = T::value;
};

template<class L>
using enabled = gate<bool_<(1 <= size<L>::value)> >;

typedef list<int, char> list_type;

int main()
{
  return enabled<list_type>::value ? 0 : 1;
}
