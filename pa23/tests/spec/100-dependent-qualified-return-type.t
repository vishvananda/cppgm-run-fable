// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.8 [temp.fct.spec]

template<unsigned long I, class T>
struct tuple;

template<unsigned long I, class T>
struct tuple_element;

template<class T>
struct tuple<0, T>
{
  T value;
};

template<class T>
struct tuple_element<0, tuple<0, T> >
{
  typedef T type;
};

template<unsigned long I, class T>
typename tuple_element<I, tuple<I, T> >::type & get(tuple<I, T> & x) noexcept
{
  return x.value;
}

int main()
{
  tuple<0, int> x;
  x.value = 9;
  return get<0>(x) == 9 ? 0 : 1;
}
