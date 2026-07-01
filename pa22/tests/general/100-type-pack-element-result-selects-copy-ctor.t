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

template<class... T>
struct tuple {};

template<unsigned long I, class... T>
struct pack_element;

template<class T, class... Rest>
struct pack_element<0, T, Rest...> {
  typedef T type;
};

template<unsigned long I, class T, class... Rest>
struct pack_element<I, T, Rest...> : pack_element<I - 1, Rest...> {
};

template<unsigned long I, class T>
struct tuple_element;

template<unsigned long I, class... T>
struct tuple_element<I, tuple<T...> > {
  typedef typename pack_element<I, T...>::type type;
};

template<unsigned long I, class... T>
typename tuple_element<I, tuple<T...> >::type & get(tuple<T...> &);

template<class T>
T && forward(typename remove_reference<T>::type &);

struct V {};

void consume(V const &);

struct Wrapped {
  explicit Wrapped(V const &) {}

  template<class T>
  explicit Wrapped(T const &value) {
    consume(forward<T>(value));
  }
};

int main() {
  tuple<V const &> t;
  Wrapped wrapped(get<0>(t));
  (void)wrapped;
  return 0;
}
