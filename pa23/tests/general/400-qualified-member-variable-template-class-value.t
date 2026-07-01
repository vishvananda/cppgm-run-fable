// VALIDATION: compile-pass
// Reducer for Boost.Asio query CPO static_query_v: a qualified class-scope
// variable-template id of class type must instantiate through the concrete
// owner and carry static-member storage metadata.

template<class...>
struct make_void {
  typedef void type;
};

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<class T>
using decay_t = T;

struct ex {};

template<int I = 0>
struct prop {
  template<class E>
  static constexpr prop static_query() {
    return prop();
  }

  template<class E, class U = decltype(prop::static_query<E>())>
  static constexpr const U static_query_v = prop::static_query<E>();
};

template<class T, class Property, class = void>
struct static_query_trait {};

template<class T, class Property>
struct static_query_trait<
    T,
    Property,
    void_t<decltype(decay_t<Property>::template static_query_v<T>)> > {
  typedef decltype(decay_t<Property>::template static_query_v<T>) result_type;

  static result_type value() {
    return decay_t<Property>::template static_query_v<T>;
  }
};

int main() {
  prop<> p = static_query_trait<ex, prop<> >::value();
  (void)p;
  return 0;
}
