// VALIDATION: compile-pass
// Boost.Exception reduction: a qualified friend function-template declaration
// should attach ADL friend access to the existing namespace template entity,
// not leave a declaration-only duplicate that hides the later definition.

namespace ns {

template<class Tag, class T>
struct error_info {
  explicit error_info(T v) : v_(v) {}
  T v_;
};

namespace detail {
struct file_tag;
struct line_tag;
}

typedef error_info<detail::file_tag, const char *> file_info;
typedef error_info<detail::line_tag, int> line_info;

namespace detail {
template<class E>
E const & set_info(E const &, file_info const &);

template<class E>
E const & set_info(E const &, line_info const &);
}

struct exception {
private:
  template<class E>
  friend E const & detail::set_info(E const &, file_info const &);
  template<class E>
  friend E const & detail::set_info(E const &, line_info const &);

  mutable const char * file_;
  mutable int line_;
};

namespace detail {
template<class E>
E const & set_info(E const & x, file_info const & y) {
  x.file_ = y.v_;
  return x;
}

template<class E>
E const & set_info(E const & x, line_info const & y) {
  x.line_ = y.v_;
  return x;
}
}

struct payload {};

struct wrapper : payload, exception {};

void make(wrapper const & w) {
  set_info(w, file_info("x"));
  set_info(w, line_info(7));
}

}

int main() {
  ns::wrapper w;
  ns::make(w);
  return 0;
}
