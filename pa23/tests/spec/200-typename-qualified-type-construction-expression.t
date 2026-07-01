// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.8 [temp.fct.spec]

template<class T>
struct traits
{
  struct cat
  {
  };
};

struct iter_ops
{
  template<class I>
  static int adv(I &, int n, const I &, typename traits<I>::cat)
  {
    return n + 1;
  }
};

template<class I>
int f(I & it, int n, const I & end)
{
  return iter_ops::adv(it, n, end, typename traits<I>::cat());
}

struct iter
{
};

int main()
{
  iter it;
  iter end;
  return f(it, 4, end) == 5 ? 0 : 1;
}
