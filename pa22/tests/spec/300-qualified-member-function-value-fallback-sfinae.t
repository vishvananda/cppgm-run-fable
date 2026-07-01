template<class T, T v>
struct integral_constant {
  static constexpr T value = v;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

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
first_t<true_type, enable_if_t<bool(Bn::value)>...> and_fn(int);

template<class...>
false_type and_fn(...);

template<class... Bn>
struct conjunction : decltype(and_fn<Bn...>(0)) {};

template<class T>
struct always_ok : true_type {};

template<bool, class... Types>
struct constraints {
  static constexpr bool ok()
  {
    return conjunction<always_ok<Types>...>::value;
  }
};

template<class T1, class T2>
struct pair_like {
  template<bool Dummy, class U1, class U2>
  using implicit_default_ctor =
      enable_if_t<constraints<Dummy, U1, U2>::ok(), bool>;

  template<bool Dummy = true,
           implicit_default_ctor<Dummy, T1, T2> = true>
  pair_like(int) {}

  pair_like(long) {}
};

struct Owner {
  struct Node;
};

struct Use {
  pair_like<Owner::Node *, int> value;
  Use() : value(0) {}
};

struct Owner::Node {};

int main() {
  Use use;
  (void)use;
  return 0;
}
