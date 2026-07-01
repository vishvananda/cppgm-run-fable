// VALIDATION: compile-pass
// A dependent base specifier that names a helper namespace from the source
// template's declaration context must keep that base when the class template is
// replayed through a reference member. Boost.MPL's or_ uses this shape, and the
// inherited type/value are needed by named-parameter SFINAE.

namespace boost
{
namespace mpl
{
template<bool V>
struct bool_
{
  enum { value = V };
  typedef bool_ type;
};

typedef bool_<true> true_;
typedef bool_<false> false_;

namespace aux
{
template<class T>
struct nested_type_wknd
{
  typedef typename T::type type;
};

template<bool C, class T1, class T2>
struct or_impl : true_
{};

template<class T1, class T2>
struct or_impl<false, T1, T2> : T2
{};
}

template<class T1, class T2>
struct or_
  : aux::or_impl<aux::nested_type_wknd<T1>::type::value, T1, T2>
{};
}

template<class A, class B>
struct is_same : mpl::false_
{};

template<class A>
struct is_same<A, A> : mpl::true_
{};

template<bool B, class T = void>
struct enable_if_c
{};

template<class T>
struct enable_if_c<true, T>
{
  typedef T type;
};

namespace nfp
{
template<class Id>
struct keyword
{
  typedef Id id;
};

template<class T, class Id>
struct typed_keyword : keyword<Id>
{
  typedef Id id;
};

template<class T, class Id, class Ref = T &>
struct named_parameter
{
  typedef T data_type;
  typedef Id id;
};

template<class NP, class Rest>
struct named_parameter_combine
{
  typedef NP first;
  typedef Rest rest;
};

template<class NP, class Keyword>
struct has_param : is_same<typename NP::id, typename Keyword::id>
{};

template<class NP, class Rest, class Keyword>
struct has_param<named_parameter_combine<NP, Rest>, Keyword>
  : mpl::or_<typename is_same<typename NP::id, typename Keyword::id>::type,
             typename has_param<Rest, Keyword>::type>
{};

template<class T, class Params, class Keyword>
typename enable_if_c<!has_param<Params, Keyword>::value, void>::type
opt_assign(T &, Params const &, Keyword)
{}

template<class T, class Params, class Keyword>
typename enable_if_c<has_param<Params, Keyword>::value, void>::type
opt_assign(T &, Params const &, Keyword)
{}
}
}

namespace outer
{
namespace
{
struct dropped;
struct kept;
struct max;
boost::nfp::typed_keyword<unsigned long, max> max_tokens;
}
}

namespace outer
{
template<class D, class C, class Ref, class Trav>
struct token_iterator_base
{
  unsigned long tokens_left;

  template<class Modifier>
  void apply_modifier(Modifier const &m)
  {
    boost::nfp::opt_assign(tokens_left, m, max_tokens);
  }
};
}

int main()
{
  typedef boost::nfp::named_parameter<int const, outer::max, int const &> M;
  typedef boost::nfp::named_parameter<char const *, outer::kept, char const *> K;
  typedef boost::nfp::named_parameter<char const *, outer::dropped, char const *> D;
  typedef boost::nfp::named_parameter_combine<M,
      boost::nfp::named_parameter_combine<K, D> > Params;

  Params params;
  outer::token_iterator_base<int, char, char const *, int> it;
  it.apply_modifier(params);
  return 0;
}
