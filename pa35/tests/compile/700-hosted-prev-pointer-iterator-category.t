#include <iterator>

const char *prev_pointer_anchor(const char *p)
{
  return std::prev(p + 1);
}

static_assert(__is_same(decltype(prev_pointer_anchor((const char *)0)), const char *),
              "std::prev preserves pointer iterator type");
