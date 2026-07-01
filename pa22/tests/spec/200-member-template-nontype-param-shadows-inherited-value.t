// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.8.2 [temp.deduct]
// A member-template non-type parameter must shadow inherited template-parameter
// value bindings while the member-template signature is collected.

namespace mpl_
{
template<unsigned long N>
struct size_t
{
};
}

namespace detail
{
typedef mpl_::size_t<1073741822> unknown_width;

template<unsigned long Width>
struct base_value
{
  static const unsigned long width = Width;
};

template<class T>
struct matcher : base_value<1>
{
};

template<class Matcher>
struct expression : Matcher
{
  static const unsigned long width = Matcher::width;

  unsigned long get_width() const
  {
    return this->get_width_(mpl_::size_t<width>());
  }

  template<unsigned long Width>
  unsigned long get_width_(mpl_::size_t<Width>) const
  {
    return Width;
  }

  unsigned long get_width_(unknown_width) const
  {
    return 0;
  }
};
}

struct traits
{
};

detail::expression<detail::matcher<traits> > *get_expr();

int main()
{
  return get_expr()->get_width() == 1 ? 0 : 1;
}
