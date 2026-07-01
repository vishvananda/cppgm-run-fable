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
struct remove_cv {
  typedef T type;
};

template<class T>
struct remove_cv<const T> {
  typedef T type;
};

template<class T>
struct remove_cv<volatile T> {
  typedef T type;
};

template<class T>
struct remove_cv<const volatile T> {
  typedef T type;
};

template<class T>
struct decay {
  typedef typename remove_cv<typename remove_reference<T>::type>::type type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class T>
decay_t<T> decay_copy(T&&) {
  return decay_t<T>();
}

struct S {
  void g();
};

void S::g() {
  struct G {};
  decay_copy(G{});
}

int main() {
  S s;
  s.g();
  return 0;
}
