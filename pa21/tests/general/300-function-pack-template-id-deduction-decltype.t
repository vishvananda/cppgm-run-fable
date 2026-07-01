template<class A, class B>
struct same {
  static const bool value = false;
};

template<class A>
struct same<A, A> {
  static const bool value = true;
};

template<class... T>
struct list {};

template<class T>
struct identity {
  typedef T type;
};

template<class... T>
struct drop_one {
  template<class... W>
  static identity<list<W...> > f(void *, identity<W> *...);

  typedef decltype(f(static_cast<identity<T> *>(0)...)) result;
  typedef typename result::type type;
};

int main()
{
  return same<drop_one<int, char>::type, list<char> >::value ? 0 : 1;
}
