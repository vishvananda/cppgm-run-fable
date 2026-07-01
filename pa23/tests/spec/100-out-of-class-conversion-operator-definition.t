// VALIDATION: compile-pass
// N3485 focus: 12.3.2 [class.conv.fct], 14.5.2 [temp.mem]

template<class T>
struct sink {};

template<class Expr>
struct wrapper
{
  typedef int result_type;
  operator sink<result_type>() const;
};

template<class Expr>
wrapper<Expr>::operator sink<result_type>() const
{
  return sink<result_type>();
}

int main()
{
  wrapper<int> w;
  sink<int> s = w;
  (void)s;
  return 0;
}
