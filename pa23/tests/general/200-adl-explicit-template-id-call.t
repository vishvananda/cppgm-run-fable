// VALIDATION: compile-pass
// Boost.MultiIndex reduction: an unqualified explicit non-type template-id call
// should use the base template name when adding ADL candidates from argument
// types.

namespace tuples
{
template<typename T0, typename T1>
struct tuple
{
  T0 first;
  T1 second;
};

template<int N, typename T0, typename T1>
int get(const tuple<T0, T1>&)
{
  return N;
}
}

struct holder
{
  tuples::tuple<int, int> key_extractors() const
  {
    return tuples::tuple<int, int>{1, 2};
  }
};

int main()
{
  holder h;
  return get<0>(h.key_extractors());
}
