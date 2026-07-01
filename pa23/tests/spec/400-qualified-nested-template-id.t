// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 14.6.2.1 [temp.dep.type], 14.5.7 [temp.alias]

template<bool B>
struct if_impl;

template<>
struct if_impl<true>
{
  template<class IfRes, class ElseRes>
  using select = IfRes;
};

typedef typename if_impl<true>::template select<int, long> selected;

int main()
{
  selected x = 4;
  return x == 4 ? 0 : 1;
}
