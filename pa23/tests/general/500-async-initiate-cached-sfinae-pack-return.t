// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.2.5 [temp.deduct.type]
// A cached no-body function-template instantiation whose trailing return
// remains dependent must not survive as a concrete overload candidate.

template<class T>
T &&declval();

template<class T>
struct type_identity {
  typedef T type;
};

template<class T>
using type_identity_t = typename type_identity<T>::type;

template<bool B, class T, class F>
struct conditional {
  typedef T type;
};

template<class T, class F>
struct conditional<false, T, F> {
  typedef F type;
};

template<bool B, class T, class F>
using conditional_t = typename conditional<B, T, F>::type;

template<class T>
struct is_const {
  static const bool value = false;
};

template<class T>
struct is_const<const T> {
  static const bool value = true;
};

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
using remove_reference_t = typename remove_reference<T>::type;

template<class T>
struct decay {
  typedef T type;
};

template<class T>
struct decay<T&> {
  typedef T type;
};

template<class T>
struct decay<T&&> {
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<bool B, class T = void>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct is_signature {
  static const bool value = false;
};

template<class R, class... Args>
struct is_signature<R(Args...)> {
  static const bool value = true;
};

template<class... Signatures>
struct are_completion_signatures;

template<>
struct are_completion_signatures<> {
  static const bool value = true;
};

template<class Signature, class... Signatures>
struct are_completion_signatures<Signature, Signatures...> {
  static const bool value =
      is_signature<Signature>::value &&
      are_completion_signatures<Signatures...>::value;
};

template<class CompletionToken, class... Signatures>
struct async_result;

template<class CompletionToken, class... Signatures>
struct has_initiate {
  static const bool value = true;
};

template<class CompletionToken,
         class... Signatures,
         class Initiation,
         class... Args>
auto async_initiate(Initiation &&initiation,
                    type_identity_t<CompletionToken> &token,
                    Args &&... args)
  -> decltype(enable_if_t<
      enable_if_t<are_completion_signatures<Signatures...>::value,
                  has_initiate<CompletionToken, Signatures...>>::value,
      async_result<decay_t<CompletionToken>, Signatures...>>::initiate(
      static_cast<Initiation&&>(initiation),
      static_cast<CompletionToken&&>(token),
      static_cast<Args&&>(args)...))
{
  return async_result<decay_t<CompletionToken>, Signatures...>::initiate(
      static_cast<Initiation&&>(initiation),
      static_cast<CompletionToken&&>(token),
      static_cast<Args&&>(args)...);
}

template<class CompletionToken,
         class... Signatures,
         class Initiation,
         class... Args>
typename enable_if_t<
    !enable_if_t<are_completion_signatures<Signatures...>::value,
                 has_initiate<CompletionToken, Signatures...>>::value,
    async_result<decay_t<CompletionToken>, Signatures...>>::return_type
async_initiate(Initiation&&, type_identity_t<CompletionToken>&, Args&&...)
{
  return 0;
}

template<class... Signatures,
         class CompletionToken,
         class Initiation,
         class... Args>
typename enable_if_t<
    !enable_if_t<are_completion_signatures<Signatures...>::value,
                 has_initiate<CompletionToken, Signatures...>>::value,
    async_result<decay_t<CompletionToken>, Signatures...>>::return_type
async_initiate(Initiation&&, CompletionToken&&, Args&&...)
{
  return 0;
}

template<class Signature, class... Values>
struct append_signature;

template<class R, class... Args, class... Values>
struct append_signature<R(Args...), Values...> {
  typedef R type(Args..., Values...);
};

struct executor {
};

struct handler {
};

template<class T, class Executor>
struct executor_binder {
  executor_binder() {
  }

  executor_binder(const Executor &ex, T &&target)
    : target_(static_cast<T&&>(target)), executor_(ex)
  {
  }

  T &get() {
    return target_;
  }

  Executor get_executor() const {
    return executor_;
  }

  T target_;
  Executor executor_;
};

template<class Executor, class T>
executor_binder<decay_t<T>, Executor> bind_executor(const Executor &ex, T &&t)
{
  return executor_binder<decay_t<T>, Executor>(ex, static_cast<T&&>(t));
}

template<class... Values>
struct value_tuple {
};

template<class... Signatures>
struct async_result<handler, Signatures...> {
  typedef int return_type;

  template<class Initiation, class RawCompletionToken, class... Args>
  static int initiate(Initiation&&, RawCompletionToken&&, Args&&...)
  {
    return 7;
  }
};

template<class T, class Executor, class Signature>
struct async_result<executor_binder<T, Executor>, Signature>
    : async_result<T, Signature> {
  template<class Initiation>
  struct init_wrapper : Initiation {
    explicit init_wrapper(Initiation init)
      : Initiation(init)
    {
    }
  };

  template<class Initiation, class RawCompletionToken, class... Args>
  static auto initiate(Initiation&&,
                       RawCompletionToken &&token,
                       Args &&... args)
    -> decltype(async_initiate<
        conditional_t<is_const<remove_reference_t<RawCompletionToken>>::value,
                      const T,
                      T>,
        Signature>(
        declval<init_wrapper<decay_t<Initiation>>>(),
        token.get(),
        token.get_executor(),
        static_cast<Args&&>(args)...))
  {
    return 9;
  }
};

template<class CompletionToken, class... Values>
struct append_t {
  CompletionToken token_;
  value_tuple<Values...> values_;
};

template<class CompletionToken, class... Values>
append_t<decay_t<CompletionToken>, decay_t<Values>...>
append(CompletionToken &&token, Values&&...)
{
  return append_t<decay_t<CompletionToken>, decay_t<Values>...>{
      static_cast<CompletionToken&&>(token),
      value_tuple<decay_t<Values>...>()};
}

template<class CompletionToken, class... Values, class Signature>
struct async_result<append_t<CompletionToken, Values...>, Signature>
    : async_result<CompletionToken,
                   typename append_signature<Signature, Values...>::type> {
  typedef typename append_signature<Signature, Values...>::type signature;

  template<class Initiation>
  struct init_wrapper : Initiation {
    explicit init_wrapper(Initiation init)
      : Initiation(init)
    {
    }
  };

  template<class Initiation, class RawCompletionToken, class... Args>
  static auto initiate(Initiation&&,
                       RawCompletionToken &&token,
                       Args &&... args)
    -> decltype(async_initiate<CompletionToken, signature>(
        declval<init_wrapper<decay_t<Initiation>>>(),
        token.token_,
        static_cast<value_tuple<Values...>&&>(token.values_),
        static_cast<Args&&>(args)...))
  {
    return 11;
  }
};

struct initiate_wait {
};

template<class Clock>
struct waitable_timer {
  template<class WaitToken>
  auto async_wait(WaitToken &&token)
    -> decltype(async_initiate<WaitToken, void(int)>(
        declval<initiate_wait>(),
        token))
  {
    return async_initiate<WaitToken, void(int)>(initiate_wait(), token);
  }
};

int main()
{
  executor ex;
  waitable_timer<int> timer;
  typedef decltype(timer.async_wait(
      append(bind_executor(ex, handler()), 1, 2))) result_type;
  (void)sizeof(result_type);
  return 0;
}
