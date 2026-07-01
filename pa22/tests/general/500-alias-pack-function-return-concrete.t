// VALIDATION: compile-pass
// A pack-indexed alias return type stays concrete for the selected function.

template<class... T>
struct tuple {};

template<unsigned long I, class... T>
struct pack_element;

template<class T, class... Rest>
struct pack_element<0, T, Rest...>
{
  typedef T type;
};

template<unsigned long I, class T, class... Rest>
struct pack_element<I, T, Rest...> : pack_element<I - 1, Rest...>
{
};

template<unsigned long I, class T>
struct tuple_element;

template<unsigned long I, class... T>
struct tuple_element<I, tuple<T...> >
{
  typedef typename pack_element<I, T...>::type type;
};

template<unsigned long I, class T>
using tuple_element_t = typename tuple_element<I, T>::type;

template<unsigned long I, class... T>
tuple_element_t<I, tuple<T...> >& get(tuple<T...>&);

struct First
{
};

struct Second
{
};

void consume(Second&);

int main()
{
  tuple<First, Second> t;
  consume(get<1>(t));
  return 0;
}
