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

struct true_type {};

template<class From, class To>
struct is_convertible {};

template<class T>
struct is_void {
  static const bool value = false;
};

template<>
struct is_void<void> {
  static const bool value = true;
};

template<class Result, class Ret, class Guard = conditional_t<is_void<Ret>::value, true_type, is_convertible<Result, Ret> > >
using call_guard_t = Guard;

template<class Result>
struct invoker {
  typedef call_guard_t<Result, void> type;
};

int main() {
  invoker<int>::type value;
  (void)value;
  return 0;
}
