namespace lib {
  template<class T>
  struct add_pointer {
    typedef T * type;
  };
}

template<class T>
using add_pointer = typename lib::add_pointer<T>::type;

template<class A, class B>
struct is_same {
  enum { value = 0 };
};

template<class A>
struct is_same<A, A> {
  enum { value = 1 };
};

template<bool C>
struct bool_constant {
  enum { value = C };
};

template<class C, class T, class E>
struct if_impl {
  typedef E type;
};

template<class T, class E>
struct if_impl<bool_constant<true>, T, E> {
  typedef T type;
};

template<class C, class T, class E>
using mp_if = typename if_impl<C, T, E>::type;

template<class... T>
struct list {};

template<template<class...> class F, class... L>
struct transform_impl {};

template<template<class...> class F, template<class...> class L, class... T>
struct transform_impl<F, L<T...> > {
  typedef L<F<T>...> type;
};

struct mismatch {};

template<template<class...> class F, class... L>
using transform =
    typename mp_if<bool_constant<true>, transform_impl<F, L...>, mismatch>::type;

struct X1 {};
struct X2 {};

typedef list<X1, X2> input;
typedef transform<lib::add_pointer, input> result;
typedef list<lib::add_pointer<X1>, lib::add_pointer<X2> > expected;

static_assert(is_same<result, expected>::value, "wrong transform result");

int main() {
  return 0;
}
