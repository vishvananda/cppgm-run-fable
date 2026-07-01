// VALIDATION: compile-pass
// N3485 focus: 14.2 [temp.names], 14.6.2.1 [temp.dep.type]

namespace meta {
template<class T>
struct holder {
  typedef T type;
};
}

struct owner {
  struct token {
    int value;
  };

  typedef meta::holder<token>::type token_type;
};

int main()
{
  owner::token_type x;
  x.value = 3;
  return x.value == 3 ? 0 : 1;
}
