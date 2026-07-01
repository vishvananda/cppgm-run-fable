template<class T, T v>
struct integral_constant {
  static const T value = v;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<class...>
using void_t = void;

template<class T>
struct pointer {
  template<class U>
  using rebind = pointer<U>;
};

template<class T, class U>
struct has_rebind {
private:
  template<class X>
  static false_type test(...);

  template<class X>
  static true_type test(typename X::template rebind<U>* = 0);

public:
  static const bool value = decltype(test<T>(0))::value;
};

template<class T, class U, bool = has_rebind<T, U>::value>
struct pointer_traits_rebind {
  typedef typename T::template rebind<U> type;
};

template<template<class, class...> class Sp, class T, class... Args, class U>
struct pointer_traits_rebind<Sp<T, Args...>, U, false> {
  typedef Sp<U, Args...> type;
};

template<class P, class = void>
struct pointer_traits_impl {};

template<class P>
struct pointer_traits_impl<P, void_t<P> > {
  typedef P pointer_type;

  template<class U>
  using rebind = typename pointer_traits_rebind<pointer_type, U>::type;
};

template<class P>
struct pointer_traits : pointer_traits_impl<P> {};

template<class T>
struct result {};

template<>
struct result<pointer<bool> > {
  static const int value = 0;
};

int main() {
  return result<pointer_traits<pointer<int> >::rebind<bool> >::value;
}
