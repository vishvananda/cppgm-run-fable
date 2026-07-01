// VALIDATION: compile-pass
// A defaulted SFINAE constructor parameter that cannot be substituted should
// drop only that constructor template candidate. A non-template copy path
// through the base class remains viable.

template<class T>
struct base {
  base()
  {}

  base(const base &)
  {}

  template<class U>
  base(base<U> &&, typename U::missing * = 0)
  {}
};

struct derived : base<int> {
  derived()
      : base<int>()
  {}

  derived(const derived &x)
      : base<int>(x)
  {}
};

int main()
{
  derived a;
  derived b(a);
  (void)b;
  return 0;
}
