#include <sstream>
#include <string>

void istream_extraction_anchor()
{
  std::istringstream in("abc");
  std::string out;
  in >> out;
}
static_assert(sizeof(&istream_extraction_anchor) > 0, "istream extraction body anchor");
