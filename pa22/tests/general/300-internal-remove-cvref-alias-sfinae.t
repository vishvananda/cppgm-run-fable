template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, class U>
struct is_not_same {
  static const bool value = true;
};

template<class T>
struct remove_reference {
  typedef T type;
};

template<class T>
struct remove_reference<T &> {
  typedef T type;
};

template<class T>
struct remove_reference<T &&> {
  typedef T type;
};

template<class T>
struct remove_cv {
  typedef T type;
};

template<class T>
struct remove_cv<const T> {
  typedef T type;
};

template<class T>
struct remove_cv<volatile T> {
  typedef T type;
};

template<class T>
struct remove_cv<const volatile T> {
  typedef T type;
};

template<class T>
using remove_cvref_t =
    typename remove_cv<typename remove_reference<T>::type>::type;

struct true_type {
  static const bool value = true;
};

struct false_type {
  static const bool value = false;
};

template<class...>
using expand_to_true = true_type;

template<class... Pred>
expand_to_true<enable_if_t<Pred::value>...> and_helper(int);

template<class...>
false_type and_helper(...);

template<class... Pred>
using and_t = decltype(and_helper<Pred...>(0));

template<class R, class... Args>
struct function;

template<class R, class... Args>
struct function<R(Args...)> {
  template<class F>
  using enable_callable =
      enable_if_t<and_t<is_not_same<remove_cvref_t<F>, function>,
                        is_not_same<F, R>>::value>;

  template<class F, class = enable_callable<F>>
  function & operator=(F);
};

struct S {
  function<void(int)> f;

  void g()
  {
    struct G {
      void operator()(int) const {}
    };
    f = G();
  }
};

int main()
{
  S s;
  s.g();
}
