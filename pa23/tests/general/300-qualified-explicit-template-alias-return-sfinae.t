// VALIDATION: compile-pass
// A class-template member body may call a namespace function template whose
// return type is selected by an alias-template SFINAE expression.

namespace N {
template<bool B, class T>
struct enable_if {
};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct is_void {
  static const bool value = false;
};

template<>
struct is_void<void> {
  static const bool value = true;
};

template<class R, class F, class A>
enable_if_t<!is_void<R>::value, R> invoke_r(F, A)
{
  return R();
}

template<class R, class F, class A>
enable_if_t<is_void<R>::value, R> invoke_r(F f, A a)
{
  f(a);
}
}

void sink(int) {}

template<class R, class F, class A>
struct Handler {
  static R call(F f, A a)
  {
    return N::invoke_r<R>(f, a);
  }
};

int main()
{
  Handler<void, void (*)(int), int>::call(sink, 1);
}
