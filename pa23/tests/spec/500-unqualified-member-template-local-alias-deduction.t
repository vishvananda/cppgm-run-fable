// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

struct state
{
};

template<bool B>
struct flag
{
};

namespace detail
{
  template<class T, bool B>
  struct matcher
  {
  };
}

template<class T>
struct compiler
{
  template<bool B>
  using matcher = detail::matcher<T, B>;

  template<bool B>
  bool term(state &, matcher<B> &);

  template<bool B>
  bool bracket(flag<B> &);
};

template<class T>
template<bool B>
bool compiler<T>::bracket(flag<B> &)
{
  state s;
  matcher<B> m;
  return term(s, m);
}

template<class T>
template<bool B>
bool compiler<T>::term(state &, matcher<B> &)
{
  return B;
}

int main()
{
  flag<true> f;
  compiler<int> c;
  return c.bracket(f) ? 0 : 1;
}
