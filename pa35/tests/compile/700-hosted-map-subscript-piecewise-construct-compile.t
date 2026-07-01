#include <map>
#include <type_traits>
static_assert(std::is_same<std::map<unsigned char, int>::key_type, unsigned char>::value, "<map> key_type");
struct V { int x; V() : x(0) {} };
int map_subscript_anchor()
{
  std::map<unsigned char, V> m;
  unsigned char k = 1;
  m[k].x = 3;
  return m[k].x;
}
static_assert(sizeof(&map_subscript_anchor) > 0, "map subscript body anchor");
