template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> { typedef T type; };

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<bool B, class T, class F>
struct conditional { typedef T type; };

template<class T, class F>
struct conditional<false, T, F> { typedef F type; };

struct failed_constraints {};

template<class First, class Second>
struct pairish {
  struct CheckArgs {
    template<int&...>
    static constexpr bool enable_explicit_default()
    {
      return false;
    }

    template<int&...>
    static constexpr bool enable_implicit_default()
    {
      return true;
    }
  };

  template<bool MaybeEnable>
  using CheckArgsDep =
      typename conditional<MaybeEnable, CheckArgs, failed_constraints>::type;

  template<bool Dummy = true,
           enable_if_t<CheckArgsDep<Dummy>::enable_explicit_default(), int> = 0>
  explicit pairish() {}

  template<bool Dummy = true,
           enable_if_t<CheckArgsDep<Dummy>::enable_implicit_default(), int> = 0>
  pairish() {}
};

int main()
{
  pairish<unsigned long, unsigned long> value;
  (void)value;
  return 0;
}
