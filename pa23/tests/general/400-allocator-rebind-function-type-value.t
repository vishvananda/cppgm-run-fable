namespace std
{
  template<class _Tp>
  struct allocator
  {
    typedef _Tp value_type;
    allocator() {}

    template<class _Up>
    allocator(const allocator<_Up>&) {}
  };

  template<class _Tp, class _Up, class = void>
  struct __has_rebind_other
  {
    static const bool value = false;
  };

  template<class _Tp,
           class _Up,
           bool = __has_rebind_other<_Tp, _Up>::value>
  struct __allocator_traits_rebind
  {
    using type =
        typename _Tp::template rebind<_Up>::other;
  };

  template<template<class, class...> class _Alloc,
           class _Tp,
           class... _Args,
           class _Up>
  struct __allocator_traits_rebind<_Alloc<_Tp, _Args...>, _Up, false>
  {
    using type = _Alloc<_Up, _Args...>;
  };

  template<class _Alloc, class _Tp>
  using __allocator_traits_rebind_t =
      typename __allocator_traits_rebind<_Alloc, _Tp>::type;

  template<class _Alloc>
  struct allocator_traits
  {
    using allocator_type = _Alloc;
    using value_type = typename allocator_type::value_type;

    template<class _Tp>
    using rebind_alloc =
        __allocator_traits_rebind_t<allocator_type, _Tp>;
  };

  template<class _Ap, class _Bp>
  struct is_same
  {
    static const bool value = false;
  };

  template<class _Ap>
  struct is_same<_Ap, _Ap>
  {
    static const bool value = true;
  };
}

typedef int (*func_ptr)(int);

template<class _Traits, class _Tp>
using rebind_alloc_t = typename _Traits::template rebind_alloc<_Tp>;

typedef std::allocator<func_ptr> alloc_type;
typedef std::allocator_traits<alloc_type> traits_type;

// Rebinding an allocator whose value_type is a function type back to its own
// value_type must yield the original allocator. Exercises structural
// substitution of a function-type template argument through the allocator_traits
// rebind alias chain (the libc++ __check_valid_allocator idiom an
// unordered_map<K, function-pointer> instantiation hits).
static_assert(
    std::is_same<
        alloc_type,
        rebind_alloc_t<traits_type, typename traits_type::value_type> >::value,
    "rebinding allocator to its own function-type value yields the same allocator");

int main()
{
  return 0;
}
