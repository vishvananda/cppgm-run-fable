// VALIDATION: compile-pass
// Template-template deduction must match F<T> without completing F<T> when a
// reference partial specialization is present and the argument type is
// incomplete.

namespace mpl_
{
template<int N>
struct int_
{
  static const int value = N;
};
}

template<int N>
struct sized_type
{
  typedef char (&type)[N];
};

namespace proto
{
struct wildcard;

template<class Expr>
struct tag_of;

namespace detail
{
sized_type<1>::type template_arity_helper(...);

template<template<class> class F, class T0>
sized_type<2>::type template_arity_helper(F<T0> **, mpl_::int_<1> *);

template<template<class, class> class F, class T0, class T1>
sized_type<3>::type template_arity_helper(F<T0, T1> **, mpl_::int_<2> *);

template<class F, int N, int Size>
struct template_arity_impl2 : mpl_::int_<Size - 1>
{
};

template<class F, int N = 2>
struct template_arity
    : template_arity_impl2<
          F,
          N,
          sizeof(detail::template_arity_helper((F **)0, (mpl_::int_<N> *)0))>
{
};

template<class F, int N>
struct template_arity_impl2<F, N, 1> : template_arity<F, N - 1>
{
};

template<class F>
struct template_arity_impl2<F, 0, 1> : mpl_::int_<-1>
{
};
}

template<class Expr>
struct tag_of
{
  typedef typename Expr::proto_tag type;
};

template<class Expr>
struct tag_of<Expr &>
{
  typedef typename Expr::proto_tag type;
};
}

static_assert(proto::detail::template_arity<
                  proto::tag_of<proto::wildcard> >::value == 1,
              "arity");

int main()
{
  return 0;
}
