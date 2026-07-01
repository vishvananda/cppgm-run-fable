template<class...>
struct make_void {
  typedef void type;
};

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<bool B>
struct bool_constant {
  static const bool value = B;
};

typedef bool_constant<true> true_type;
typedef bool_constant<false> false_type;

template<class T>
struct Wrap {
  typedef int iterator_category;
  static int noisy() { return T::missing_name; }
};

template<class T>
struct has_cat {
  template<class U>
  static false_type test(...);

  template<class U>
  static true_type test(void_t<typename U::iterator_category>* = nullptr);

  static const bool value = decltype(test<T>(nullptr))::value;
};

static_assert(has_cat<Wrap<int> >::value, "");

int main() {
  return 0;
}
