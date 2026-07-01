template<class T1, class T2>
struct pair_like {
  template<class Check = void, int = 0>
  explicit(false) pair_like() : first(), second() {}

  T1 first;
  T2 second;
};

template<class U1, class U2>
struct holder {
  pair_like<U1, U2> value;
};

struct piecewise_construct_t {
  explicit piecewise_construct_t() = default;
};

inline constexpr piecewise_construct_t piecewise_construct = piecewise_construct_t();

template<class T>
struct aligned_as_t {};

template<class... Args>
struct max_align_impl : aligned_as_t<Args>... {};

struct D {};

const unsigned long k = alignof(max_align_impl<unsigned long long, double, long double, D, int*>);

template<class T, T V>
struct integral_constant {
  static constexpr T value = V;
};

template<class X>
struct iterator_traits;

template<class X>
using iterator_category_t = typename iterator_traits<X>::difference_type;

template<class Default, template<class> class Op, class Arg>
using detected_or_t = Default;

struct nat {};

template<class T, class U>
struct outer {
  struct inner : integral_constant<bool, __is_convertible(detected_or_t<nat, iterator_category_t, T>, U)> {};
};

int main() {
  return 0;
}
