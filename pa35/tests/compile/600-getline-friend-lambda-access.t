#include <sstream>
#include <string>

void getline_anchor()
{
  std::istringstream in("abc");
  std::string out;
  std::getline(in, out);
}
static_assert(sizeof(&getline_anchor) > 0, "getline body anchor");
