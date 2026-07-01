// VALIDATION: compile-pass
// N3485 focus: 5.19 [expr.const], 7.1.5 [dcl.constexpr], 14.5.3 [temp.variadic]

typedef decltype(sizeof(0)) size_t;

constexpr size_t first_true_loop(bool const * first, bool const * last)
{
  size_t index = 0;
  while(first != last && !*first)
  {
    ++index;
    ++first;
  }
  return index;
}

template<size_t N>
struct value
{
  static const size_t result = N;
};

template<class... T>
struct find_first_true
{
  static constexpr bool values[] = { T::value... };
  typedef value<first_true_loop(values, values + sizeof...(T))> type;
};

struct no
{
  static const bool value = false;
};

struct yes
{
  static const bool value = true;
};

typedef find_first_true<no, no, yes, no>::type result;
typedef char check_result[result::result == 2 ? 1 : -1];

int main()
{
  return 0;
}
