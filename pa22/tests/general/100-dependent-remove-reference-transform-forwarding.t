template<class T>
struct remove_reference {
  typedef T type;
};

template<class T>
struct remove_reference<T &> {
  typedef T type;
};

template<class T>
struct remove_reference<T &&> {
  typedef T type;
};

template<class T>
using remove_reference_t = typename remove_reference<T>::type;

template<class T>
remove_reference_t<T>&& move(T&& t) {
  return static_cast<remove_reference_t<T>&&>(t);
}

template<class F>
struct stored {
  explicit stored(F&&) {}
  explicit stored(const F&) {}
};

struct value_func {
  template<class P>
  explicit value_func(P&& p) {
    stored<P> s(move(p));
  }
};

template<class F>
struct function {
  function(F f) : value(move(f)) {}
  value_func value;
};

struct S {
  void g();
};

void S::g() {
  struct G {};
  function<G> f(G{});
}

int main() {
  S s;
  s.g();
  return 0;
}
