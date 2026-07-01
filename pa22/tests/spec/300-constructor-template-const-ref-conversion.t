// VALIDATION: compile-pass
// N3485 focus: 14.8 [temp.fct.spec]

template<int N>
struct tag {};

template<class Rep, class Period>
struct duration
{
  Rep value;

  duration(Rep r) : value(r) {}

  template<class Rep2, class Period2>
  duration(const duration<Rep2, Period2> & other) : value(other.value) {}
};

int accept(const duration<long, tag<1> > & d)
{
  return d.value == 8 ? 0 : 1;
}

int main()
{
  return accept(duration<int, tag<2> >(8));
}
