// VALIDATION: compile-pass
// N3485 focus: 5.19 [expr.const], 7.1.5 [dcl.constexpr], 14.5.3 [temp.variadic]

typedef decltype(sizeof(0)) size_t;

constexpr size_t first_true(bool const * first, bool const * last)
{
  return first == last || *first ? 0 : 1 + first_true(first + 1, last);
}

template<class T>
struct is_long
{
  static constexpr bool value = false;
};

template<>
struct is_long<long>
{
  static constexpr bool value = true;
};

template<class... T>
struct find_long
{
  static constexpr bool values[] = { is_long<T>::value... };
  typedef char found_at_one[first_true(values, values + sizeof...(T)) == 1 ? 1 : -1];
};

typedef find_long<int, long, char>::found_at_one selected;

int main()
{
  return 0;
}
