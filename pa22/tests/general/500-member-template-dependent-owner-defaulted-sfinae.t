// VALIDATION: compile-pass
// A trailing decltype in a function template can instantiate an inner member
// template through a dependent source owner before the body calls the same
// member template through the concrete owner. Defaulted SFINAE parameters must
// see the concrete owner's template arguments and typedefs, not the dependent
// source-owner aliases.

template <bool B, class T = void>
struct enable_if {
};

template <class T>
struct enable_if<true, T> {
  typedef T type;
};

template <bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template <bool B, class T, class F>
struct conditional {
  typedef T type;
};

template <class T, class F>
struct conditional<false, T, F> {
  typedef F type;
};

template <bool B, class T, class F>
using conditional_t = typename conditional<B, T, F>::type;

template <class T>
struct remove_reference {
  typedef T type;
};

template <class T>
struct remove_reference<T&> {
  typedef T type;
};

template <class T>
struct remove_reference<T&&> {
  typedef T type;
};

template <class T>
using decay_t = typename remove_reference<T>::type;

template <bool B>
struct bool_constant {
  static const bool value = B;
};

struct executor {
};

template <class T>
struct is_executor : bool_constant<false> {
};

template <>
struct is_executor<executor> : bool_constant<true> {
};

template <class T, class U>
struct is_same : bool_constant<false> {
};

template <class T>
struct is_same<T, T> : bool_constant<true> {
};

template <class Handler, class Executor>
struct associated_executor {
  typedef void unspecialised;
};

struct empty_work_function {
};

template <class Function, class Handler, class Executor, class = void>
struct needs_work : bool_constant<true> {
};

template <class Handler, class Executor>
struct needs_work<empty_work_function, Handler, Executor,
    enable_if_t<is_same<typename associated_executor<Handler, Executor>::unspecialised,
      void>::value> > : bool_constant<false> {
};

struct handler {
};

template <class Initiation, class RawCompletionToken, class... Args>
int async_result_initiate(Initiation&& initiation,
    RawCompletionToken&& token, Args&&... args)
{
  return static_cast<Initiation&&>(initiation)(
      static_cast<RawCompletionToken&&>(token),
      static_cast<Args&&>(args)...);
}

template <class T>
T&& declval();

template <class Executor>
struct initiate {
  typedef Executor executor_type;

  explicit initiate(const Executor&) {
  }

  template <class CompletionHandler, class Function>
  int operator()(CompletionHandler&&, Function&&,
      enable_if_t<is_executor<conditional_t<true, executor_type, CompletionHandler> >::value>* = 0,
      enable_if_t<!needs_work<decay_t<Function>, decay_t<CompletionHandler>, Executor>::value>* = 0) const
  {
    return 1;
  }

  template <class CompletionHandler, class Function>
  int operator()(CompletionHandler&&, Function&&,
      enable_if_t<is_executor<conditional_t<true, executor_type, CompletionHandler> >::value>* = 0,
      enable_if_t<needs_work<decay_t<Function>, decay_t<CompletionHandler>, Executor>::value>* = 0) const
  {
    return 2;
  }

  template <class CompletionHandler, class Function>
  int operator()(CompletionHandler&&, Function&&,
      enable_if_t<!is_executor<conditional_t<true, executor_type, CompletionHandler> >::value>* = 0,
      enable_if_t<!needs_work<decay_t<Function>, decay_t<CompletionHandler>, Executor>::value>* = 0) const
  {
    return 3;
  }

  template <class CompletionHandler, class Function>
  int operator()(CompletionHandler&&, Function&&,
      enable_if_t<!is_executor<conditional_t<true, executor_type, CompletionHandler> >::value>* = 0,
      enable_if_t<needs_work<decay_t<Function>, decay_t<CompletionHandler>, Executor>::value>* = 0) const
  {
    return 4;
  }
};

struct context {
  typedef executor executor_type;

  executor get_executor()
  {
    return executor();
  }
};

template <class ExecutionContext, class NullaryToken>
auto post(ExecutionContext& ctx, NullaryToken&& token)
    -> decltype(async_result_initiate(
        declval<initiate<typename ExecutionContext::executor_type> >(),
        token, empty_work_function()))
{
  return async_result_initiate(
      initiate<typename ExecutionContext::executor_type>(ctx.get_executor()),
      static_cast<NullaryToken&&>(token), empty_work_function());
}

int main()
{
  handler h;
  context ctx;
  return post(ctx, static_cast<handler&&>(h)) == 1 ? 0 : 1;
}
