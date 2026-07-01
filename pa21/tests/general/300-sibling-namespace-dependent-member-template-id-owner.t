namespace root {
namespace detail {
struct marker {};
}

namespace lib {
struct outer {
  struct detail {
    template<class Sig, class Enable = void>
    struct result_detail {};

    template<class This, class First, class Second>
    struct result_detail<This(First, Second), void> {
      typedef int type;
    };
  };

  template<class Sig>
  struct result {
    typedef typename detail::result_detail<Sig>::type type;
  };
};
}
}

struct C {};

typedef root::lib::outer outer_type;
typedef outer_type::result<outer_type(C &, int const &)>::type result_type;

int main() {
  result_type value = 0;
  return value;
}
