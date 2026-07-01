// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type]

namespace tuples
{
struct null_type
{
};

template<
  class T0 = null_type,
  class T1 = null_type,
  class T2 = null_type,
  class T3 = null_type,
  class T4 = null_type,
  class T5 = null_type,
  class T6 = null_type,
  class T7 = null_type,
  class T8 = null_type,
  class T9 = null_type>
struct tuple
{
  tuple(T0, T1, T2)
  {
  }
};
}

template<class T0, class... Ts>
int take(const tuples::tuple<T0, Ts...> &)
{
  return sizeof...(Ts);
}

int main()
{
  tuples::tuple<int, int, int> value(0, 0, 0);
  return take(value) == 2 ? 0 : 1;
}
