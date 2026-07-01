// VALIDATION: compile-pass
// Alias SFINAE must preserve inherited member values used by noexcept and
// decltype probes when selecting a converting constructor template.

template<bool B, class T>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, T V>
struct integral_constant {
  static const T value = V;
};

template<bool B>
using __bool_constant = integral_constant<bool, B>;

template<bool B>
struct bool_holder {
  static const bool value = B;
};

typedef bool_holder<true> true_type;
typedef bool_holder<false> false_type;

template<class T>
struct voider {
  typedef void type;
};

template<class T>
struct type_identity {
  typedef T type;
};

template<class Result, class Ret, bool IsVoid = false, class = void>
struct invocable_impl : false_type {
  typedef false_type type;
};

template<class Result, class Ret>
struct invocable_impl<Result, Ret, false, typename voider<typename Result::type>::type> {
  typedef typename Result::type result_type;

  static result_type get() noexcept;

  template<class T>
  static void conv(typename type_identity<T>::type) noexcept;

  template<class T,
           bool Nothrow = noexcept(conv<T>(get())),
           class = decltype(conv<T>(get())),
           bool Dangle = false>
  static __bool_constant<Nothrow && !Dangle> test(int);

  template<class T, bool = false>
  static false_type test(...);

  typedef decltype(test<Ret, true>(1)) type;
};

template<class T>
struct success_type {
  typedef T type;
};

template<class F>
struct result_of_impl {
  typedef success_type<int> type;
};

template<class F>
struct invoke_result : result_of_impl<F>::type {};

int one() { return 1; }

template<class Sig>
struct function;

template<class R>
struct function<R()> {
  template<class F, class DF = F, class Res2 = invoke_result<DF&> >
  struct callable : invocable_impl<Res2, R>::type {};

  template<class Cond, class T = void>
  using requires_t = enable_if_t<Cond::value, T>;

  template<class F, class = requires_t<callable<F> > >
  function(F&&) {}
};

int main()
{
  function<int()> f = one;
  return 0;
}
