template<class... T>
struct list {};

template<class A, class B>
struct is_same {
  static const bool value = false;
};

template<class A>
struct is_same<A, A> {
  static const bool value = true;
};

template<class T>
struct identity {
  typedef T type;
};

template<class... T>
struct probe {
  template<class... W>
  static identity<list<W...> > f(identity<W>*...);

  typedef decltype(f(static_cast<identity<T>*>(0)...)) result;
  typedef typename result::type type;
};

struct A {};
struct B {};

typedef typename probe<A, B>::type actual;
typedef list<A, B> expected;

static_assert(is_same<actual, expected>::value, "deduced trailing pack");

int main() {
  return 0;
}
