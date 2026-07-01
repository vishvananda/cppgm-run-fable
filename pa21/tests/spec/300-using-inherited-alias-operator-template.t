// VALIDATION: compile-pass
// Reduced from Boost.Proto via Boost.Xpressive.
// A using-declaration may name an inherited type alias as the qualifier for an
// inherited member operator template while the derived class reference members
// are still being collected.

template<class Expr, class Derived, class Domain, long Arity = 0>
struct extends {
  typedef extends proto_extends;

  template<class A>
  int operator=(A const &)
  {
    return 7;
  }
};

struct expr {};
struct domain {};

struct mark_tag : extends<expr, mark_tag, domain> {
  typedef mark_tag::proto_extends proto_extends;
  using proto_extends::operator=;
};

int main()
{
  mark_tag mark;
  return (mark = 1) == 7 ? 0 : 1;
}
