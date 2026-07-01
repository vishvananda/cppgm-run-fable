// VALIDATION: compile-pass
// A std-shaped disjunction alias with dependent trait arguments must stay
// dependent while collecting an enable_if_t default argument.

namespace std {
template<class T, T V>
struct integral_constant {
  static const T value = V;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<class A, class B>
struct is_same : false_type {
};

template<class A>
struct is_same<A, A> : true_type {
};

template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class...>
struct __or_detail : false_type {
};

template<class B1>
struct __or_detail<B1> : B1 {
};

template<bool, class B1, class... Bn>
struct __or_step;

template<class B1, class... Bn>
struct __or_step<true, B1, Bn...> : true_type {
};

template<class B1, class... Bn>
struct __or_step<false, B1, Bn...> : __or_detail<Bn...> {
};

template<class B1, class... Bn>
struct __or_detail<B1, Bn...> : __or_step<B1::value, B1, Bn...> {
};

template<class... Bn>
using _Or = __or_detail<Bn...>;

template<class T, class... Options>
using __is_any_of = _Or<is_same<T, Options>...>;

template<class T>
using __sort_like_supported = __is_any_of<T, char, int>;

template<class T, enable_if_t<__sort_like_supported<T>::value, int> = 0>
int pick(T)
{
  return 1;
}

template<class T, enable_if_t<!__sort_like_supported<T>::value, int> = 0>
long pick(T)
{
  return 2;
}
}

static_assert(sizeof(std::pick(1)) == sizeof(int),
              "matching type selects enabled overload");
static_assert(sizeof(std::pick(1L)) == sizeof(long),
              "non-matching type keeps fallback overload viable");

int main()
{
  return sizeof(std::pick(1L)) == sizeof(long) ? 0 : 1;
}
