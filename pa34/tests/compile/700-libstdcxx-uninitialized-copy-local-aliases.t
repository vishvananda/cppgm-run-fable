namespace __gnu_cxx {
  template<class Iterator, class Container>
  class __normal_iterator {
  public:
    Iterator base() const;
  };
}

namespace __gnu_debug {
  template<class Iterator, class Sequence, class Category>
  class _Safe_iterator;
}

namespace std {
  typedef decltype(sizeof(0)) size_t;

  struct random_access_iterator_tag {};

  template<class T>
  T&& declval();

  template<class T, T v>
  struct integral_constant {
    static const T value = v;
    constexpr operator T() const { return v; }
  };

  typedef integral_constant<bool, true> true_type;
  typedef integral_constant<bool, false> false_type;

  template<class T>
  struct type_identity {
    typedef T type;
  };

  template<class T>
  struct iterator_traits;

  template<class T>
  struct iterator_traits<T*> {
    typedef T value_type;
  };

  template<class T>
  struct iterator_traits<const T*> {
    typedef T value_type;
  };

  template<class T>
  struct remove_pointer {
    typedef T type;
  };

  template<class T>
  struct remove_pointer<T*> {
    typedef T type;
  };

  template<class T>
  struct remove_pointer<const T*> {
    typedef T type;
  };

  template<class T, class U>
  struct constructible {
    static const bool value = false;
  };

  template<class T, size_t = sizeof(T)>
  constexpr true_type is_complete_or_unbounded(type_identity<T>) {
    return true_type();
  }

  template<class TypeIdentity, class NestedType = typename TypeIdentity::type>
  constexpr false_type is_complete_or_unbounded(TypeIdentity) {
    return false_type();
  }

  template<class T>
  struct is_nothrow_copy_constructible {
    static_assert(is_complete_or_unbounded(type_identity<T>()),
                  "template argument must be complete or unbounded");
    static const bool value = true;
  };

  template<class Dest, class Src>
  struct memcpyable {
    static const bool value = true;
  };

  template<class Iterator, class Container>
  Iterator __niter_base(__gnu_cxx::__normal_iterator<Iterator, Container> it)
      noexcept(is_nothrow_copy_constructible<Iterator>::value) {
    return it.base();
  }

  template<class T>
  T __niter_base(T it) noexcept(is_nothrow_copy_constructible<T>::value) {
    return it;
  }

  template<class Iterator, class Sequence>
  decltype(std::__niter_base(std::declval<Iterator>()))
  __niter_base(const ::__gnu_debug::_Safe_iterator<Iterator,
                                                  Sequence,
                                                  std::random_access_iterator_tag>&)
      noexcept(is_nothrow_copy_constructible<Iterator>::value);

  template<class InputIterator, class ForwardIterator>
  ForwardIterator uninitialized_copy(InputIterator first,
                                     InputIterator last,
                                     ForwardIterator result) {
    using Dest = decltype(std::__niter_base(result));
    using Src = decltype(std::__niter_base(first));
    using ValT = typename iterator_traits<ForwardIterator>::value_type;
    if constexpr (!constructible<ValT, decltype(*first)>::value) {
      (void)last;
      return result;
    } else if constexpr (memcpyable<Dest, Src>::value) {
      size_t n = last - first;
      if (n > 0) {
        using PtrValT = typename remove_pointer<Src>::type;
        (void)sizeof(PtrValT);
      }
      return result;
    }
    return result;
  }
}

namespace semantic_conversion {
int run() {
  struct Candidate {};
  Candidate source[1];
  Candidate target[1];
  std::uninitialized_copy((const Candidate*)source,
                          (const Candidate*)source + 1,
                          target);
  return 0;
}
}

int main() {
  return semantic_conversion::run();
}
