// Reduced from Boost.MP11 mp_transform through mp_flatten_impl<L>::fn. A
// template-template parameter can bind a member alias template, and applying it
// to a type whose spelling is an empty class template-id must still resolve.

namespace n {

template<class... T>
struct list {};

namespace detail {

template<template<class...> class F, class L>
struct transform_impl {};

template<template<class...> class F, template<class...> class L, class... T>
struct transform_impl<F, L<T...> >
{
  typedef L<F<T>...> type;
};

template<class L2>
struct flatten
{
  template<class T>
  using fn = T;
};

} // namespace detail

typedef detail::transform_impl<
    detail::flatten<n::list<> >::fn,
    n::list<n::list<> > >::type result;

} // namespace n

int main()
{
  return 0;
}
