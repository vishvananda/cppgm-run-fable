template<bool B>
struct bool_
{
  static const bool value = B;
};

template<bool B, typename P0>
struct and_2 : bool_<P0::value>
{};

template<typename P0>
struct and_2<false, P0> : bool_<false>
{};

struct terminal_tag;
struct sequence_tag;
struct any_;
struct char_type;

template<typename T>
struct term
{
  typedef T child0;
};

template<typename A0, typename A1>
struct list2
{
  typedef A0 child0;
  typedef A1 child1;
};

template<typename Tag, typename Args, long Arity>
struct basic_expr
{
  typedef Tag proto_tag;
  typedef Args proto_args;
  typedef basic_expr proto_grammar;
  typedef basic_expr proto_derived_expr;
};

template<typename Tag, typename Args, long Arity>
struct expr
{
  typedef Tag proto_tag;
  typedef Args proto_args;
  typedef basic_expr<Tag, Args, Arity> proto_grammar;
  typedef expr proto_derived_expr;
};

template<typename Expr>
struct expr_traits
{
  typedef Expr value_type;
};

template<typename Expr>
struct expr_traits<Expr const &>
{
  typedef Expr value_type;
};

template<typename Cases>
struct switch_
{
  typedef switch_ proto_grammar;
};

template<typename Char, typename Gram>
struct cases
{
  template<typename Tag, typename Dummy = void>
  struct case_ : bool_<false>
  {};

  template<typename Dummy>
  struct case_<terminal_tag, Dummy>
    : basic_expr<terminal_tag, term<any_>, 0>
  {};

  template<typename Dummy>
  struct case_<sequence_tag, Dummy>
    : basic_expr<sequence_tag, list2<Gram, Gram>, 2>
  {};
};

template<typename Char>
struct grammar : switch_<cases<Char, grammar<Char> > >
{};

template<typename Expr, typename BasicExpr, typename Grammar>
struct matches_;

template<typename T, typename U>
struct terminal_matches : bool_<false>
{};

template<typename T>
struct terminal_matches<T, T> : bool_<true>
{};

template<typename T>
struct terminal_matches<T, any_> : bool_<true>
{};

template<typename Expr, typename Tag, typename Args1, typename Args2>
struct matches_<Expr, basic_expr<Tag, Args1, 0>, basic_expr<Tag, Args2, 0> >
  : terminal_matches<typename Args1::child0, typename Args2::child0>
{};

template<typename Expr, typename Tag, typename Args, long Arity, typename Cases>
struct matches_<Expr, basic_expr<Tag, Args, Arity>, switch_<Cases> >
  : matches_<
        Expr,
        basic_expr<Tag, Args, Arity>,
        typename Cases::template case_<Tag>::proto_grammar
    >
{};

template<typename Expr, typename Tag, typename Args1, long N1, typename Args2, long N2>
struct matches_<Expr, basic_expr<Tag, Args1, N1>, basic_expr<Tag, Args2, N2> >
  : bool_<false>
{};

template<typename Expr, typename Tag, typename Args1, typename Args2>
struct matches_<Expr, basic_expr<Tag, Args1, 2>, basic_expr<Tag, Args2, 2> >
  : and_2<
        matches_<
            typename expr_traits<typename Args1::child0>::value_type::proto_derived_expr,
            typename expr_traits<typename Args1::child0>::value_type::proto_grammar,
            typename Args2::child0::proto_grammar
        >::value,
        matches_<
            typename expr_traits<typename Args1::child1>::value_type::proto_derived_expr,
            typename expr_traits<typename Args1::child1>::value_type::proto_grammar,
            typename Args2::child1::proto_grammar
        >
    >
{};

struct left_token;
struct right_token;

typedef expr<terminal_tag, term<left_token>, 0> left_expr;
typedef expr<terminal_tag, term<right_token>, 0> right_expr;
typedef expr<sequence_tag, list2<left_expr const &, right_expr const &>, 2> sequence_expr;
typedef grammar<char_type> grammar_type;
typedef basic_expr<sequence_tag, list2<grammar_type, grammar_type>, 2> sequence_grammar;

static_assert(matches_<left_expr,
                       typename left_expr::proto_grammar,
                       typename grammar_type::proto_grammar>::value,
              "left");
static_assert(matches_<right_expr,
                       typename right_expr::proto_grammar,
                       typename grammar_type::proto_grammar>::value,
              "right");
static_assert(matches_<
                  typename sequence_expr::proto_derived_expr,
                  typename sequence_expr::proto_grammar,
                  sequence_grammar
              >::value,
              "sequence direct");
static_assert(matches_<sequence_expr,
                       typename sequence_expr::proto_grammar,
                       typename grammar_type::proto_grammar>::value,
              "sequence switch");

int main()
{
  return 0;
}
