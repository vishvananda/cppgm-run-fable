template<class Matcher, class Next>
struct static_xpression
{
  typedef Matcher matcher_type;
  typedef Next next_type;
};

template<class Expr, class State, class Data>
struct transform_impl
{
  typedef Expr const expr;
  typedef Data const data;
};

template<class Expr, class State, class Data>
struct transform_impl<Expr &, State, Data &>
{
  typedef Expr expr;
  typedef Data data;
};

template<class Tag>
struct visitor
{
  template<class Matcher>
  struct apply
  {
    typedef Matcher type;
  };
};

struct as_matcher
{
  template<class Expr, class State, class Data>
  struct impl : transform_impl<Expr, State, Data>
  {
    typedef typename impl::data data_type;
    typedef typename data_type::template apply<typename impl::expr>::type result_type;
  };
};

template<class Grammar>
struct in_sequence
{
  template<class Expr, class State, class Data>
  struct impl : transform_impl<Expr, State, Data>
  {
    typedef static_xpression<
        typename Grammar::template impl<Expr, State, Data>::result_type,
        State> result_type;
  };
};

struct expr {};
struct state {};
struct tag {};

typedef in_sequence<as_matcher>::impl<expr const &, state, visitor<tag> &>::result_type sequence_type;
typedef sequence_type::matcher_type matcher_type;
