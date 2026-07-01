template<class...>
struct make_void {
  typedef void type;
};

template<class... Ts>
using void_t = typename make_void<Ts...>::type;

template<class T>
T&& declval();

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

struct identity {
  template<class T>
  T&& operator()(T&& t) const;
};

template<class, class F, class... Args>
struct invoke_result_impl {
};

template<class F, class... Args>
struct invoke_result_impl<void_t<decltype(declval<F>()(declval<Args>()...))>, F, Args...> {
  typedef decltype(declval<F>()(declval<Args>()...)) type;
};

template<class F, class... Args>
using invoke_result_t = typename invoke_result_impl<void, F, Args...>::type;

typedef invoke_result_t<identity&, bool const&> warm_bool;
typedef invoke_result_t<identity&, void*&> void_pointer_result;

static_assert(is_same<warm_bool, bool const&>::value, "bool warmup");
static_assert(is_same<void_pointer_result, void*&>::value, "member template result should use current pack element");

int main()
{
  return is_same<void_pointer_result, void*&>::value ? 0 : 1;
}
