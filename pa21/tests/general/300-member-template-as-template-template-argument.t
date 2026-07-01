// VALIDATION: compile-pass
// A qualified member template can be used as a template-template argument.

template<template<class> class TT>
struct use
{
  typedef typename TT<int>::type type;
};

struct quote
{
  template<class T>
  struct fn
  {
    typedef T type;
  };
};

typedef typename use<quote::template fn>::type result_t;

template<class A, class B>
struct same
{
  static const bool value = false;
};

template<class A>
struct same<A, A>
{
  static const bool value = true;
};

static_assert(same<result_t, int>::value, "");

int main()
{
  return 0;
}
