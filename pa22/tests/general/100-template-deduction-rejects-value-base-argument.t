namespace meta
{
template<bool B>
struct bool_
{
  enum { value = B };
};

typedef bool_<true> true_;

template<int N>
struct int_
{
  enum { value = N };
};

namespace aux
{
template<class T>
struct type_wrapper
{};

template<int N>
struct arity_tag
{
  typedef char (&type)[(unsigned)N + 1];
};

template<int C1, int C2>
struct max_arity
{
  enum { value = C2 > 0 ? C2 : (C1 > 0 ? C1 : -1) };
};

arity_tag<0>::type arity_helper(...);

template<template<class> class F, class T1>
typename arity_tag<1>::type arity_helper(type_wrapper<F<T1> >, arity_tag<1>);

template<template<class, class> class F, class T1, class T2>
typename arity_tag<2>::type arity_helper(type_wrapper<F<T1, T2> >, arity_tag<2>);

template<class F, int N>
struct template_arity_impl
{
  enum { value = sizeof(arity_helper(type_wrapper<F>(), arity_tag<N>())) - 1 };
};

template<class F>
struct template_arity
{
  enum {
    value = max_arity<
        template_arity_impl<F, 1>::value,
        template_arity_impl<F, 2>::value>::value
  };
  typedef int_<value> type;
};
}
}

template<class A, class B>
struct is_same
{};

template<class A>
struct is_same<A, A> : meta::true_
{};

namespace placeholders
{
template<int N>
struct arg;

template<>
struct arg<-1>
{};
}

typedef is_same<placeholders::arg<-1>, placeholders::arg<-1> > same_placeholder;

static_assert(meta::aux::template_arity<same_placeholder>::value == 2, "arity");

int main()
{
  return 0;
}
