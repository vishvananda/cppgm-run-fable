// VALIDATION: compile-pass
// N3485 focus: 10.2 [class.member.lookup], 13.3.1.1.2 [over.call.object], 14.5.2 [temp.mem]

struct base_fn
{
  template<class T>
  void operator()(T &) const
  {
  }
};

struct derived_fn : base_fn
{
  template<class T>
  void operator()(T & value)
  {
    base_fn::operator()(value);
  }
};

template<class T>
struct iter
{
  T * p;

  T & operator*() const
  {
    return *p;
  }
};

template<class Tag>
struct unrolled
{
  template<class I0, class F>
  static void call(I0 const & i0, F & fn)
  {
    fn(*i0);
  }
};

int main()
{
  int value = 0;
  iter<int> it = { &value };

  base_fn base;
  unrolled<void>::call(it, base);

  derived_fn derived;
  unrolled<void>::call(it, derived);

  return value;
}
