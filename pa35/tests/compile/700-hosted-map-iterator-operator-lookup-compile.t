#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
static_assert(std::is_same<std::map<std::string, std::shared_ptr<int> >::mapped_type, std::shared_ptr<int> >::value, "map mapped_type");
using namespace std;
typedef map<string, shared_ptr<int> > Table;
bool missing(Table & table, const string & key)
{ Table::const_iterator found = table.find(key); return found == table.end(); }
bool map_iterator_lookup_anchor()
{
  Table table;
  return missing(table, "x");
}
static_assert(sizeof(&map_iterator_lookup_anchor) > 0, "map iterator lookup body anchor");
