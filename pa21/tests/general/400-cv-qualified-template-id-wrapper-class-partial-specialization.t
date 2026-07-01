// VALIDATION: compile-pass
// A top-level cv wrapper must beat a template-id partial specialization.

template<class T>
struct holder
{
  static const int value = 0;
};

template<class T>
struct holder<T const>
{
  static const int value = 1;
};

template<template<class...> class L, class... T>
struct holder<L<T...> >
{
  static const int value = 2;
};

template<class T, class U>
struct box
{
};

static_assert(holder<box<int, long> >::value == 2, "");
static_assert(holder<box<int, long> const>::value == 1, "");

int main()
{
  return holder<box<int, long> const>::value == 1 ? 0 : 1;
}
