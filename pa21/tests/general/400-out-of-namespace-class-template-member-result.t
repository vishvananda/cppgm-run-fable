namespace ns {
template<int N>
struct child {
  template<class Expr>
  struct impl {
    typedef typename Expr::left result_type;
  };
};

template<int N>
struct expr;
}

template<int N>
struct ns::expr {
  typedef expr<N - 1> left;
  enum { value = N };
};

template<>
struct ns::expr<0> {
  enum { value = 0 };
};

typedef ns::child<0>::impl<ns::expr<2> >::result_type result_type;

int main()
{
  return result_type::value == 1 ? 0 : 1;
}
