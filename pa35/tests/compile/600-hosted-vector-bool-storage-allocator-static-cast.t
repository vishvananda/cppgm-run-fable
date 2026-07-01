#include <vector>

unsigned long vector_bool_size_anchor()
{
  std::vector<bool> bits(4);
  return bits.size();
}
static_assert(sizeof(&vector_bool_size_anchor) > 0, "vector<bool> body anchor");
