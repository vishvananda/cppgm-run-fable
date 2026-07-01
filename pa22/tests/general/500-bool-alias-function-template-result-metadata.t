template<class T, T V>
struct integral_constant {
  typedef T value_type;
  static const T value = V;
  operator value_type() const { return value; }
};

template<bool V>
using bool_constant = integral_constant<bool, V>;

template<class T>
struct result_type {
  typedef T type;
};

template<class Tp, bool Nothrow = true, class = void>
bool_constant<Nothrow> test_result(int);

template<class Tp, bool = false>
integral_constant<bool, false> test_result(...);

template<class Result, class Ret, bool = false, class = void>
struct invocable_impl : integral_constant<bool, false> {};

template<class Result, class Ret>
struct invocable_impl<Result, Ret, false, void> {
  typedef typename Result::type result;
  typedef decltype(test_result<Ret, true>(1)) type;
};

struct target {};

int main() {
  invocable_impl<result_type<target *>, target *>::type value;
  return value ? 0 : 1;
}
