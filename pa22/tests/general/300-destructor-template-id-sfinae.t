// VALIDATION: compile-pass
// A destructor call in a dependent decltype expression should participate in
// SFINAE without requiring the probed class-template-id to be written as a
// simple identifier.

template<class T>
T && declval();

struct true_type
{
  static const bool value = true;
};

struct false_type
{
  static const bool value = false;
};

struct destructible_impl
{
  template<class T, class = decltype(declval<T &>().~T())>
  static true_type test(int);

  template<class>
  static false_type test(...);
};

template<class T>
struct is_destructible : destructible_impl
{
  typedef decltype(test<T>(0)) type;
  static const bool value = type::value;
};

namespace ns
{
inline namespace v1
{
template<class T>
struct String
{
  T *p;
};
}

namespace detail
{
template<class A, class B>
struct Pair
{
  A first;
  B second;
};

template<class T, bool Constant, bool Cache>
struct Iter
{
  typedef T value_type;
  Iter() = default;
};
}
}

static_assert(
    is_destructible<
        ns::detail::Iter<
            ns::detail::Pair<const ns::String<char>, unsigned long int>,
            false,
            true> >::value,
    "template-id destructor target should resolve to the class destructor");

int main()
{
  return 0;
}
