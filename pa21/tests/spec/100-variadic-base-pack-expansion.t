struct one
{
  static const int value = 1;
};

struct two
{
  static const int value = 2;
};

template<typename... Ts>
struct sum_bases;

template<>
struct sum_bases<>
{
  int sum() const
  {
    return 0;
  }
};

template<typename Head, typename... Tail>
struct sum_bases<Head, Tail...> : Head, sum_bases<Tail...>
{
  int sum() const
  {
    return Head::value + sum_bases<Tail...>::sum();
  }
};

int main()
{
  sum_bases<one, two> value;
  return value.sum() == 3 ? 0 : 1;
}
