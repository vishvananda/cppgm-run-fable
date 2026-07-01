template<class T>
struct decay
{
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class Sig, class Functor>
struct handler;

template<class Result, class Functor, class... Args>
struct handler<Result(Args...), Functor>
{
  static Result invoke(Args...) { return 11; }
};

template<class Sig>
struct function_box;

template<class Result, class... Args>
struct function_box<Result(Args...)>
{
  typedef Result (*invoker_type)(Args...);

  template<class Functor>
  using handler_type = handler<Result(Args...), decay_t<Functor> >;

  template<class Functor>
  explicit function_box(Functor)
  {
    typedef handler_type<Functor> selected_handler;
    invoker_type invoker;
    invoker = &selected_handler::invoke;
    (void)invoker;
  }
};

template<class T>
struct token {};
struct callable {};

int main()
{
  function_box<int(const token<char> &, const token<char> &)> value((callable()));
  (void)value;
  return 0;
}
