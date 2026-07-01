#include <utility>

void local_pointer_pair_anchor()
{
  []() {
    struct Local
    {
      int value;
    };
    using hosted_pair = std::pair<Local *, Local *>;
    using constructed_pair = decltype(hosted_pair((Local *)0, (Local *)0));
    static_assert(__is_same(constructed_pair, hosted_pair), "local pointer pair construction is unambiguous");
  }();
}
