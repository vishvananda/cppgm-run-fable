#include <vector>

struct Outer {
  struct PPToken {
    int value;
  };

  struct Macro {
    Macro() {}
    std::vector<PPToken> tokens;
  };

  std::vector<PPToken> cur_arg;
};

unsigned long nested_vector_anchor()
{
  Outer o;
  return o.cur_arg.size();
}
static_assert(sizeof(&nested_vector_anchor) > 0, "nested vector constructor body anchor");
