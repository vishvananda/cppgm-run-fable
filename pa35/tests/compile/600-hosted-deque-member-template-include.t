#include <deque>

unsigned long deque_size_anchor()
{
  std::deque<int> values;
  return values.size();
}
static_assert(sizeof(&deque_size_anchor) > 0, "deque member body anchor");
