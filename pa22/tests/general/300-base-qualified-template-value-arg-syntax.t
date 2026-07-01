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

template<class...>
using void_t = void;

template<class T>
T&& declval();

template<class T>
struct remove_reference {
  typedef T type;
};

template<class T>
struct remove_reference<T&> {
  typedef T type;
};

template<class T>
struct remove_reference<T&&> {
  typedef T type;
};

template<class T>
struct is_member_object_pointer : false_type {};

template<class T>
struct is_member_function_pointer : false_type {};

struct invoke_other {};

template<class T, class Tag>
struct result_of_success {
  typedef T type;
  typedef Tag invoke_type;
};

struct failure_type {};

template<bool, bool, class Functor, class... ArgTypes>
struct result_of_impl {
  typedef failure_type type;
};

struct result_of_other_impl {
  template<class Fn, class... Args>
  static result_of_success<
      decltype(declval<Fn>()(declval<Args>()...)),
      invoke_other> test(int);

  template<class...>
  static failure_type test(...);
};

template<class Functor, class... ArgTypes>
struct result_of_impl<false, false, Functor, ArgTypes...>
    : private result_of_other_impl {
  typedef decltype(test<Functor, ArgTypes...>(0)) type;
};

template<class Functor, class... ArgTypes>
struct invoke_result
    : result_of_impl<
          is_member_object_pointer<typename remove_reference<Functor>::type>::value,
          is_member_function_pointer<typename remove_reference<Functor>::type>::value,
          Functor,
          ArgTypes...>::type {};

template<class Result, class Ret, bool = false, class = void>
struct is_invocable_impl : false_type {};

template<class Result, class Ret>
struct is_invocable_impl<Result, Ret, true, void_t<typename Result::type> >
    : true_type {};

template<class Sig>
struct function;

template<class R, class... Args>
struct function<R(Args...)> {
  template<class F,
           class D = F,
           class R2 = invoke_result<D&, Args...> >
  struct Callable : is_invocable_impl<R2, R, true>::type {};

  template<class Cond, class T = void>
  using Requires = enable_if_t<Cond::value, T>;

  template<class F>
  Requires<Callable<F>, function&> operator=(F&&) { return *this; }
};

struct S {
  function<void(int)> f;
  void g()
  {
    struct G {
      void operator()(int) const {}
    };
    f = G{};
  }
};

int main()
{
  S s;
  s.g();
}
