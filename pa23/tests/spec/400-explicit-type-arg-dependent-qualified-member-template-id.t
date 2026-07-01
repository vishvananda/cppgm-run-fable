// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.3.1 [temp.arg.type]

namespace boost
{
template<bool B, class T = void>
struct enable_if_c
{
  typedef T type;
};

template<class T>
struct enable_if_c<false, T>
{
};
}

namespace boost
{
namespace nfp
{
namespace nfp_detail
{
template<class Derived>
struct named_parameter_base
{
  int marker() const
  {
    return 1;
  }
};
}

template<class A, class B>
struct same
{
  static const bool value = false;
};

template<class A>
struct same<A, A>
{
  static const bool value = true;
};

template<class Id, bool Required = false>
struct keyword
{
  typedef Id id;
};

template<class T, class Id, class RefType = T &>
struct named_parameter
{
  typedef Id id;
  typedef T data_type;
  typedef RefType ref_type;
  T value;
};

template<class NP, class Rest>
struct named_parameter_combine
  : nfp_detail::named_parameter_base<named_parameter_combine<NP, Rest> >
{
  using nfp_detail::named_parameter_base<
      named_parameter_combine<NP, Rest> >::marker;

  NP value;
};

template<class Params, class Keyword>
struct has_param : same<typename Params::id, typename Keyword::id>
{
};

template<class Params, class NP>
typename boost::enable_if_c<
    !has_param<Params, keyword<typename NP::id> >::value,
    named_parameter_combine<NP, Params> >::type
opt_append(Params const &, NP const & np)
{
  named_parameter_combine<NP, Params> result;
  result.value = np;
  return result;
}

template<class Params, class NP>
typename boost::enable_if_c<
    has_param<Params, keyword<typename NP::id> >::value,
    Params>::type
opt_append(Params const & params, NP const &)
{
  return params;
}
}
}

namespace boost
{
namespace runtime
{
namespace
{
struct first_id
{
};

struct second_id
{
};
}

int run_case()
{
  nfp::named_parameter<int, first_id, int const &> first;
  nfp::named_parameter<int, second_id, int const &> second;
  first.value = 3;
  second.value = 7;
  nfp::named_parameter_combine<
      nfp::named_parameter<int, second_id, int const &>,
      nfp::named_parameter<int, first_id, int const &> > combined =
          nfp::opt_append(first, second);
  return combined.value.value == 7 && combined.marker() == 1 ? 0 : 1;
}
}
}

int main()
{
  return boost::runtime::run_case();
}
