// VALIDATION: compile-pass

template<class T>
struct type_tag {};

template<class PMF, class R, class T>
struct member_fn
{
  PMF f;

  explicit member_fn(PMF f_) : f(f_) {}
};

template<class R, class T>
member_fn<R (T::*)(), R, T> mem_fn(R (T::* f)())
{
  return member_fn<R (T::*)(), R, T>(f);
}

template<class R, class F, class A>
struct bound
{
  F f;
  A a;

  bound(F f_, A a_) : f(f_), a(a_) {}
};

template<class R, class F, class A>
bound<R, F, A> bind(type_tag<R>, F f, A a)
{
  return bound<R, F, A>(f, a);
}

template<class Rt2, class R, class T, class A>
auto bind(type_tag<Rt2>, R (T::* f)(), A a)
    -> decltype(bind(type_tag<Rt2>(), mem_fn(f), a))
{
  return bind(type_tag<Rt2>(), mem_fn(f), a);
}

struct X
{
  int f0()
  {
    return 7;
  }
};

int main()
{
  X x;
  typedef decltype(bind(type_tag<void>(), mem_fn(&X::f0), &x)) expected_type;
  expected_type selected = bind(type_tag<void>(), &X::f0, &x);
  return selected.a == &x ? 0 : 1;
}
