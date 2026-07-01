struct true_type {
  static const bool value = true;
};

struct false_type {
  static const bool value = false;
};

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, class...>
using first_t = T;

template<class... Bn>
auto and_fn(int) -> first_t<true_type, enable_if_t<bool(Bn::value)>...>;

template<class... Bn>
auto and_fn(...) -> false_type;

template<class... Bn>
struct and_ : decltype(and_fn<Bn...>(0)) {};

template<class T>
struct is_good : true_type {};

struct A {};
struct B {};

static_assert(and_<is_good<A>, is_good<B> >::value, "");

int main()
{
  return 0;
}
