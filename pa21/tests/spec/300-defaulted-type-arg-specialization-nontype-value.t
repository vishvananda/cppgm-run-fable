// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype], 14.7.3 [temp.expl.spec]

namespace mpl_
{
  namespace aux
  {
  }

  struct na
  {
  };

  template<int N>
  struct int_
  {
    static const int value = N;
  };
}

namespace boost
{
namespace mpl
{
  using ::mpl_::int_;
  using ::mpl_::na;

  namespace aux
  {
    using namespace ::mpl_::aux;

    template<class F>
    struct template_arity;
  }

  template<class T = na,
           class Tag = na,
           class Arity = int_<aux::template_arity<T>::value> >
  struct lambda
  {
    typedef T type;
  };

  namespace aux
  {
    template<class T>
    struct type_wrapper
    {
    };

    template<int N>
    struct arity_tag
    {
      typedef char (&type)[N + 1];
    };

    template<int C1, int C2>
    struct max_arity
    {
      static const int value = C2 > 0 ? C2 : (C1 > 0 ? C1 : -1);
    };

    arity_tag<0>::type arity_helper(...);

    template<template<class P1> class F, class T1>
    typename arity_tag<1>::type arity_helper(type_wrapper<F<T1> >, arity_tag<1>);

    template<template<class P1, class P2> class F, class T1, class T2>
    typename arity_tag<2>::type arity_helper(type_wrapper<F<T1, T2> >,
                                             arity_tag<2>);

    template<class F, int N>
    struct template_arity_impl
    {
      static const int value =
          sizeof(::boost::mpl::aux::arity_helper(type_wrapper<F>(),
                                                 arity_tag<N>())) - 1;
    };

    template<class F>
    struct template_arity
    {
      static const int value =
          max_arity<template_arity_impl<F, 1>::value,
                    template_arity_impl<F, 2>::value>::value;
      typedef int_<value> type;
    };
  }

  template<>
  struct lambda<na, na>
  {
    template<class T1, class T2, class T3 = na, class T4 = na, class T5 = na>
    struct apply : lambda<T1, T2>
    {
    };
  };
}
}

int main()
{
  boost::mpl::lambda<mpl_::na, mpl_::na>::apply<int, mpl_::na> value;
  (void)&value;
  return 0;
}
