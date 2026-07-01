#include <algorithm>
#include <string>
#include <vector>
#include <type_traits>

static_assert(std::is_same<std::vector<int>::value_type, int>::value, "<vector> value_type");
static_assert(std::is_same<std::string::value_type, char>::value, "<string> value_type");

std::string f1()
{
  struct Local { std::string s; int x; };
  std::vector<Local> v(2);
  std::sort(v.begin(), v.end(), [](const Local& a, const Local& b) { return a.s < b.s; });
  return std::string();
}
std::string f2()
{
  struct Local { std::string s; int x; };
  std::vector<Local> v(2);
  std::sort(v.begin(), v.end(), [](const Local& a, const Local& b) { return a.s < b.s; });
  return std::string();
}
static_assert(sizeof(&f1) > 0, "first local-class sort body anchor");
static_assert(sizeof(&f2) > 0, "second local-class sort body anchor");
