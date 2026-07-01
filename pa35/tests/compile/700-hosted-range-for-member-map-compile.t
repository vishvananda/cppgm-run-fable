#include <map>
#include <string>
#include <type_traits>
static_assert(std::is_same<std::map<std::string,int>::value_type, std::pair<const std::string,int> >::value, "<map> value_type");
struct NamedValues { std::map<std::string, int> values; };
int sum_named_values(const NamedValues & named)
{ int sum = 0; for(const auto & entry : named.values) { sum += entry.second; } return sum; }
static_assert(sizeof(&sum_named_values) > 0, "range-for map body anchor");
