// VALIDATION: compile-pass
// A member-template assignment candidate dropped by SFINAE must leave the
// ordinary copy-assignment fallback viable.

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, class U>
struct is_same {
  static const bool value = false;
};

template<class T>
struct is_same<T, T> {
  static const bool value = true;
};

template<class Sig>
struct function;

template<class R, class... Args>
struct function<R(Args...)> {
  function& operator=(const function&) { return *this; }

  template<class F>
  enable_if_t<!is_same<F, function>::value, function&> operator=(const F&);
};

int main()
{
  function<void()> lhs;
  function<void()> rhs;
  lhs = rhs;
  return 0;
}
