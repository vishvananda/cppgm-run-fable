// VALIDATION: compile-pass

template<class T, T... I>
struct integer_sequence
{
};

template<unsigned long... I>
using index_sequence = integer_sequence<unsigned long, I...>;

template<class... T>
using index_sequence_for = index_sequence<0, 1>;

template<class Indices, class... T>
struct tuple_impl;

template<unsigned long... I, class... T>
struct tuple_impl<index_sequence<I...>, T...>
{
  enum { value = sizeof...(I) };
};

template<class... T>
struct tuple
{
  typedef tuple_impl<index_sequence_for<T...>, T...> BaseT;
  BaseT base;
};

int main()
{
  static_assert(tuple<int &&, char &&>::BaseT::value == 2, "");
  return tuple<int &&, char &&>::BaseT::value == 2 ? 0 : 1;
}
