template<class T> T&& declval();

template<class...>
using void_t = void;

template<class, class... Args>
struct invoke_result_impl {
};

template<class... Args>
struct invoke_result_impl<
    void_t<decltype(__builtin_invoke(declval<Args>()...))>,
    Args...> {
  typedef decltype(__builtin_invoke(declval<Args>()...)) type;
};

template<class... Args>
using invoke_result_t = typename invoke_result_impl<void, Args...>::type;

template<class... Args>
invoke_result_t<Args...> invoke(Args&&... args)
{
  return __builtin_invoke(args...);
}

template<class... Args>
struct is_callable {
  enum { value = sizeof(invoke_result_t<Args...>) >= 0 };
};

struct Item {
  int value;
};

template<class F, class A, class B>
int call_checked(F func, A& item, B& needle)
{
  enum { callable = is_callable<F&, A&, B&>::value };
  return invoke(func, item, needle) ? callable : 0;
}

int main()
{
  Item item = {4};
  int needle = 3;
  auto less_than_member = [](Item& current, int& value) {
    return value < current.value;
  };
  return call_checked(less_than_member, item, needle) ? 0 : 1;
}
