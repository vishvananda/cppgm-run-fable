// VALIDATION: compile-pass
// Repeated identical packs are more specialized than independently matched packs.

template<class... T>
struct list
{
};

template<class T1, class T2>
struct similar_impl
{
  static const int value = 0;
};

template<template<class...> class L, class... T1, class... T2>
struct similar_impl<L<T1...>, L<T2...> >
{
  static const int value = 1;
};

template<template<class...> class L, class... T>
struct similar_impl<L<T...>, L<T...> >
{
  static const int value = 2;
};

static_assert(similar_impl<list<>, list<> >::value == 2, "");
static_assert(similar_impl<list<int>, list<int> >::value == 2, "");
static_assert(similar_impl<list<int>, list<long> >::value == 1, "");
static_assert(similar_impl<list<>, list<list<> > >::value == 1, "");

int main()
{
  return 0;
}
