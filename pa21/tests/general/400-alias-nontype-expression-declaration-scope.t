typedef unsigned long size_t;

namespace meta {

template<class T, T V>
struct constant {
  static const T value = V;
};

template<class... T>
struct list {};

template<class L>
struct size_impl;

template<template<class...> class L, class... T>
struct size_impl<L<T...> > {
  typedef constant<size_t, sizeof...(T)> type;
};

template<class L>
using size = typename size_impl<L>::type;

template<bool B>
using bool_ = constant<bool, B>;

template<class L, size_t N>
using check = bool_<(N <= size<L>::value)>;

}

int main()
{
  return meta::check<meta::list<int, char>, 1>::value ? 0 : 1;
}
