template<bool B>
struct bool_ {
  static const bool value = B;
};

typedef bool_<true> true_;
typedef bool_<false> false_;

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

template<class A, class B>
using two = A;

int main() {
  if(valid<two>::value) {
    return 1;
  }
  if(valid<two, int>::value) {
    return 2;
  }
  if(!valid<two, int, long>::value) {
    return 3;
  }
  return 0;
}
