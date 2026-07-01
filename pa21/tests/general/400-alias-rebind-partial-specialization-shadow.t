// VALIDATION: compile-pass

namespace mini
{
template<class T>
struct allocator
{
  typedef T value_type;

  allocator()
  {
  }

  template<class U>
  allocator(const allocator<U> &)
  {
  }
};

template<class Alloc, class T, bool HasRebind = false>
struct rebind
{
  typedef typename Alloc::template rebind<T>::other type;
};

template<template<class, class...> class Alloc, class T, class... Args, class U>
struct rebind<Alloc<T, Args...>, U, false>
{
  typedef Alloc<U, Args...> type;
};

template<class Alloc>
struct allocator_traits
{
  template<class T>
  using rebind_alloc = typename rebind<Alloc, T>::type;
};
}

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

int main()
{
  typedef mini::allocator_traits<mini::allocator<int> >::rebind_alloc<void *> rebound;
  static_assert(is_same<rebound, mini::allocator<void *> >::value, "");
  return 0;
}
