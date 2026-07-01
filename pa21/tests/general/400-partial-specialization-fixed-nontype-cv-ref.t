// VALIDATION: compile-pass

template<class Expr, long N>
struct child_c
{
  static const int value = 0;
};

template<class Expr>
struct child_c<Expr, 1>
{
  static const int value = 1;
};

template<class Expr>
struct child_c<Expr &, 1>
{
  static const int value = 2;
};

template<class Expr>
struct child_c<Expr const &, 1>
{
  static const int value = 3;
};

struct X
{
};

static_assert(child_c<X const &, 1>::value == 3, "");

int main()
{
  return child_c<X const &, 1>::value == 3 ? 0 : 1;
}
