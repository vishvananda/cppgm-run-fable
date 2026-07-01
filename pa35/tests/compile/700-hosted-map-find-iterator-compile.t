#include <map>
#include <string>
#include <type_traits>
static_assert(std::is_same<std::map<std::string,int>::mapped_type, int>::value, "<map> mapped_type");
static_assert(std::is_same<std::map<std::string,int>::key_type, std::string>::value, "<map> key_type");
bool map_find_anchor()
{
  std::map<std::string, int> m;
  return m.find(std::string()) == m.end();
}
static_assert(sizeof(&map_find_anchor) > 0, "map find body anchor");
