// VALIDATION: compile-pass
// N3485 focus: 14.5.4 [temp.friend], 14.6.4 [temp.dep.res]

struct value
{
  int n;
};

struct adl
{
  int delta;

  template<class T>
  friend int apply(T & x, const adl & a)
  {
    x.n = x.n + a.delta;
    return x.n;
  }
};

int main()
{
  value x;
  adl a;
  x.n = 4;
  a.delta = 5;
  return apply(x, a) == 9 && x.n == 9 ? 0 : 1;
}
