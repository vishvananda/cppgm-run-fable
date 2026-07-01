// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 5.4 [expr.cast], 7 [dcl.dcl]

namespace meta {
template<class T>
struct make_unsigned {
  typedef unsigned int type;
};
}

struct ctype_base {
  static const unsigned int space = 1;
  static const unsigned int regex_word = 0x80;

  static_assert((regex_word &
                 ~(meta::make_unsigned<unsigned int>::type)(space)) ==
                    regex_word, "");
};

int main()
{
  return (meta::make_unsigned<unsigned int>::type)(1) == 1 ? 0 : 1;
}
