// VALIDATION: compile-pass
// A hidden friend function template declared inside a class template must keep
// the enclosing template parameters when probed through expression SFINAE.

template<bool B, typename T = void>
struct enable_if {};

template<typename T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, typename T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<typename T>
T&& declval();

template<typename...>
using void_t = void;

template<typename T, T V>
struct integral_constant {
  static constexpr T value = V;
};

typedef integral_constant<bool, true> true_type;
typedef integral_constant<bool, false> false_type;

template<typename A, typename B>
struct is_same : false_type {};

template<typename A>
struct is_same<A, A> : true_type {};

namespace query_fn {
void query();
}

namespace test {
using query_fn::query;

struct context_t {};

template<typename T, typename P, typename = void>
struct query_member : false_type {};

template<typename T, typename P>
struct query_member<T, P, void_t<decltype(declval<T>().query(declval<P>()))>>
    : true_type {};

template<typename T, typename P, typename = void>
struct query_free : false_type {};

template<typename T, typename P>
struct query_free<T, P, void_t<decltype(query(declval<T>(), declval<P>()))>>
    : true_type {};

template<typename T, typename P>
struct can_query : integral_constant<bool,
    query_member<T, P>::value || query_free<T, P>::value> {};

template<typename T>
struct context_as_t {
  template<typename Executor, typename U>
  friend constexpr U query(const Executor& ex, const context_as_t<U>&,
      enable_if_t<is_same<T, U>::value>* = 0,
      enable_if_t<can_query<const Executor&, const context_t&>::value>* = 0)
  {
    return ex.query(context_t());
  }
};

struct executor {
  int& query(context_t) const;
};

static_assert(can_query<const executor&, const context_t&>::value, "");
static_assert(query_free<executor, context_as_t<int&>>::value, "");
static_assert(can_query<executor, context_as_t<int&>>::value, "");
}

int main() {
  return test::can_query<test::executor, test::context_as_t<int&>>::value ? 0 : 1;
}
