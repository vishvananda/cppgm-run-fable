// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], substitution failure in a dependent
// trailing-return decltype removes the function template candidate.

template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

struct supported {
};

struct unsupported {
};

template<class T>
struct is_supported {
  static const bool value = false;
};

template<>
struct is_supported<supported> {
  static const bool value = true;
};

template<class T>
T && declval();

struct wrapper {
  template<class Property>
  int require(const Property &,
              typename enable_if<is_supported<Property>::value, int>::type = 0) const
  {
    return 1;
  }
};

template<class T, class Property>
struct can_require {
  template<class U, class P>
  static char test(decltype(declval<U>().require(declval<P>())) *);

  template<class, class>
  static long test(...);

  static const bool value = sizeof(test<T, Property>(0)) == sizeof(char);
};

template<bool B, class T, class U>
struct conditional {
  typedef T type;
};

template<class T, class U>
struct conditional<false, T, U> {
  typedef U type;
};

template<bool B, class T, class U>
using conditional_t = typename conditional<B, T, U>::type;

template<class T>
struct proxy {
  struct type {
    template<class P>
    auto require(P && p)
      -> decltype(declval<conditional_t<true, T, P> >().require(static_cast<P &&>(p)));
  };
};

template<class T, class Property>
struct can_require_proxy {
  template<class U, class P>
  static char test(decltype(declval<typename proxy<U>::type>().require(declval<P>())) *);

  template<class, class>
  static long test(...);

  static const bool value = sizeof(test<T, Property>(0)) == sizeof(char);
};

static_assert(can_require<wrapper, supported>::value,
              "supported property should work");
static_assert(!can_require<wrapper, unsupported>::value,
              "unsupported property should SFINAE out");
static_assert(can_require_proxy<wrapper, supported>::value,
              "supported proxy property should work");
static_assert(!can_require_proxy<wrapper, unsupported>::value,
              "unsupported proxy property should SFINAE out through trailing return");

int main()
{
  return can_require_proxy<wrapper, unsupported>::value ? 1 : 0;
}
