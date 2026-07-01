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

state *get_state();

template<class T, bool B>
detail::matcher<T, B> *get_matcher();

flag<true> *get_flag();

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
  state *s = get_state();
  matcher<B> *m = get_matcher<T, B>();
  return term(*s, *m);
}

template<class T>
template<bool B>
bool compiler<T>::term(state &, matcher<B> &)
{
  return B;
}

compiler<int> *get_compiler();

int main()
{
  return get_compiler()->bracket(*get_flag()) ? 0 : 1;
}
