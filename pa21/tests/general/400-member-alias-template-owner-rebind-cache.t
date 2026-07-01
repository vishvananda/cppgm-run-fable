// Reduced from Boost.Container allocator_traits.  A member alias template body
// can contain another member alias from the same class template.  When the
// outer alias is instantiated for a different owner, the inner dependent alias
// must be looked up in that owner rather than reusing the first instantiated
// owner's alias declaration.

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

template<class T>
struct standard_allocator {
  typedef T value_type;

  template<class U>
  struct rebind {
    typedef standard_allocator<U> other;
  };
};

template<class T>
struct simple_allocator {
  typedef T value_type;
};

template<class Ptr, class U, bool HasRebind>
struct pointer_rebinder;

template<class Ptr, class U>
struct pointer_rebinder<Ptr, U, true> {
  typedef typename Ptr::template rebind<U>::other type;
};

template<template<class> class Ptr, class A, class U>
struct pointer_rebinder<Ptr<A>, U, false> {
  typedef Ptr<U> type;
};

template<class T>
struct has_rebind {
  static const bool value = false;
};

template<class T>
struct has_rebind<standard_allocator<T> > {
  static const bool value = true;
};

template<class Ptr, class U>
struct pointer_rebind
    : pointer_rebinder<Ptr, U, has_rebind<Ptr>::value> {
};

template<class Allocator>
struct allocator_traits {
  typedef Allocator allocator_type;

  template<class T>
  using rebind_alloc = typename pointer_rebind<Allocator, T>::type;

  template<class T>
  using rebind_traits = allocator_traits<rebind_alloc<T> >;
};

void instantiate_standard_owner()
{
  allocator_traits<standard_allocator<void> >::rebind_traits<double> traits;
  (void)traits;
}

static_assert(is_same<
              allocator_traits<simple_allocator<int> >::rebind_traits<double>::allocator_type,
              simple_allocator<double> >::value,
              "member alias owner");

int main()
{
  instantiate_standard_owner();
  return 0;
}
