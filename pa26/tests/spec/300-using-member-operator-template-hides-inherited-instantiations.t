// VALIDATION: compile-pass
// N3485 focus: 7.3.3 [namespace.udecl], 10.2 [class.member.lookup], 13.5 [over.oper], 14.5.2 [temp.mem]

template<class NP, class Rest>
struct combine;

template<class Derived>
struct comma_base
{
  template<class NP>
  combine<NP, Derived>
  operator,(NP const & np) const
  {
    return combine<NP, Derived>(
        np,
        *static_cast<Derived const *>(this));
  }
};

struct first : comma_base<first>
{
};

struct second
{
};

struct third
{
};

struct fourth
{
};

template<class NP, class Rest>
struct combine
  : Rest
  , comma_base<combine<NP, Rest> >
{
  NP value;

  combine(NP const & np, Rest const & rest)
    : Rest(rest)
    , value(np)
  {
  }

  using comma_base<combine<NP, Rest> >::operator,;
};

int main()
{
  first a;
  second b;
  third c;
  fourth d;

  combine<fourth, combine<third, combine<second, first> > > result =
      (a, b, c, d);
  (void)result;

  return 0;
}
