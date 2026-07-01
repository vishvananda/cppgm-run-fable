// VALIDATION: compile-pass
// An inherited constructor template keeps deduction and defaulted enable-if
// participation when the derived type is a parameter-pack specialization.

template <bool B, class T = void>
struct enable_if {};

template <class T>
struct enable_if<true, T> {
  typedef T type;
};

template <class A, class B>
struct is_same {
  static const bool value = false;
};

template <class A>
struct is_same<A, A> {
  static const bool value = true;
};

template <class T>
struct remove_reference {
  typedef T type;
};

template <class T>
struct remove_reference<T &> {
  typedef T type;
};

template <class T>
struct remove_reference<T &&> {
  typedef T type;
};

struct first {};
struct second {};
struct third {};

template <class... Ts>
struct tuple_like {
private:
  struct tag {};

  tuple_like(tag, const Ts &...) {}

  template <class... Args>
  struct accepts_impl {
    static const bool value = true;
  };

  template <class First, class... Rest>
  struct accepts_impl<First, Rest...> {
    static const bool value =
        !is_same<typename remove_reference<First>::type, tag>::value;
  };

public:
  template <class... Args>
  using accepts = accepts_impl<Args...>;

  template <class... Args,
            typename enable_if<accepts<Args &&...>::value, int>::type = 0>
  tuple_like(Args &&... args)
      : tuple_like(tag(), static_cast<Args &&>(args)...) {}
};

template <class... Ts>
struct key : private tuple_like<Ts...> {
  using tuple_like<Ts...>::tuple_like;
};

int main()
{
  first a;
  second b;
  third c;
  key<first, second, third> value(a, b, c);
  (void)value;
  return 0;
}
