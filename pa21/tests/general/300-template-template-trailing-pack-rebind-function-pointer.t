// VALIDATION: compile-pass

template<class T>
struct allocator
{
  typedef T value_type;
};

template<class A, class U>
struct rebind;

template<template<class, class...> class Alloc, class T, class... Args, class U>
struct rebind<Alloc<T, Args...>, U>
{
  typedef Alloc<U, Args...> type;
};

template<class A, class B>
struct is_same
{
  static const bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static const bool value = true;
};

typedef int (*function_pointer)(int);

static_assert(
    is_same<
        allocator<function_pointer>,
        rebind<allocator<function_pointer>, function_pointer>::type>::value,
    "");

int main()
{
  return 0;
}
