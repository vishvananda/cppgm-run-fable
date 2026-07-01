// VALIDATION: compile-pass
// N3485 focus: 13.5 [over.oper], 14.5.2 [temp.mem]

template<class NP, class Rest>
struct named_parameter_combine;

template<class Derived>
struct named_parameter_base
{
  template<class NP>
  named_parameter_combine<NP, Derived>
  operator,(NP const & np) const
  {
    return named_parameter_combine<NP, Derived>(
        np,
        *static_cast<Derived const *>(this));
  }
};

template<class T, class Id>
struct named_parameter : named_parameter_base<named_parameter<T, Id> >
{
  T value;
};

template<class NP, class Rest>
struct named_parameter_combine
  : Rest
  , named_parameter_base<named_parameter_combine<NP, Rest> >
{
  NP param;

  named_parameter_combine(NP const & np, Rest const & rest)
    : Rest(rest)
    , param(np)
  {
  }
};

template<class Params, class NP>
named_parameter_combine<NP, Params>
append(Params const & params, NP const & np)
{
  return (params, np);
}

struct first_id
{
};

struct second_id
{
};

int main()
{
  named_parameter<int, first_id> first;
  named_parameter<int, second_id> second;
  first.value = 3;
  second.value = 7;

  named_parameter_combine<
      named_parameter<int, second_id>,
      named_parameter<int, first_id> > combined = append(first, second);

  return combined.value == 3 && combined.param.value == 7 ? 0 : 1;
}
