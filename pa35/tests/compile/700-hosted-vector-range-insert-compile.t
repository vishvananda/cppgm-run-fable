#include <vector>
#include <type_traits>
static_assert(std::is_same<std::vector<int>::value_type, int>::value, "<vector> value_type");
void vector_range_insert_anchor()
{
  std::vector<int> a;
  std::vector<int> b;
  a.insert(a.begin(), b.begin(), b.end());
}
static_assert(sizeof(&vector_range_insert_anchor) > 0, "vector range insert body anchor");
