template<class T, T v>
struct integral_constant {
  static const T value = v;
  typedef integral_constant type;
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

template<class T>
struct is_void : false_type {};

template<>
struct is_void<void> : true_type {};

template<class T, class U>
struct is_same : false_type {};

template<class T>
struct is_same<T, T> : true_type {};

struct nat {};

namespace ns {
template<class T>
T&& declval();

template<class F, class... Args>
decltype(ns::declval<F>()(ns::declval<Args>()...)) invoke(F&&, Args&&...);
}

template<class Ret, class F, class... Args>
struct invokable_r {
  template<class XF, class... XArgs>
  static decltype(ns::invoke(ns::declval<XF>(), ns::declval<XArgs>()...)) try_call(int);

  template<class XF, class... XArgs>
  static nat try_call(...);

  typedef decltype(try_call<F, Args...>(0)) Result;
  static const bool value =
      !is_same<Result, nat>::value && is_void<Ret>::value;
};

template<class Ret, class F, class... Args>
struct is_invocable_r
    : integral_constant<bool, invokable_r<Ret, F, Args...>::value> {};

template<class...>
using expand_to_true = true_type;

template<class... Pred>
expand_to_true<enable_if_t<Pred::value>...> and_helper(int);

template<class...>
false_type and_helper(...);

template<class... Pred>
using And = decltype(and_helper<Pred...>(0));

template<class Sig>
struct function;

template<class R, class... Args>
struct function<R(Args...)> {
  function() {}

  template<class F,
           class = typename enable_if<
               And<is_invocable_r<R, F&, Args...> >::value>::type>
  function(F) {}

  template<class F,
           class = typename enable_if<
               And<is_invocable_r<R, F&, Args...> >::value>::type>
  function& operator=(F&&) { return *this; }
};

struct S {
  function<void(int)> f;
  void g();
};

void S::g() {
  struct G {
    void operator()(int) const {}
  };
  f = G{};
}

int main() {
  S s;
  s.g();
  return 0;
}
