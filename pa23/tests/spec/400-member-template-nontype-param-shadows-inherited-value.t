// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.8.2 [temp.deduct]
// A member-template non-type parameter must shadow inherited template-parameter
// value bindings while the member-template signature is collected.

namespace mpl_ {
template<unsigned long N>
struct size_t {
};
}

namespace detail {
typedef mpl_::size_t<1073741822> unknown_width;

struct result {
  explicit result(unsigned long v = 0) : value(v) {}
  unsigned long value;
};

template<unsigned long Width>
struct base_value {
  static const unsigned long width = Width;
};

template<class T>
struct matcher : base_value<1> {
};

template<class Matcher>
struct expression : Matcher {
  static const unsigned long width = Matcher::width;

  result get_width() const {
    return this->get_width_(mpl_::size_t<width>());
  }

  template<unsigned long Width>
  result get_width_(mpl_::size_t<Width>) const {
    return result(Width);
  }

  result get_width_(unknown_width) const {
    return result(0);
  }
};
}

struct traits {
};

int main()
{
  detail::expression<detail::matcher<traits> > expr;
  return expr.get_width().value == 1 ? 0 : 1;
}
