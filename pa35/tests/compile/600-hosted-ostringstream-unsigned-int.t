#include <sstream>

void ostringstream_unsigned_anchor()
{
  std::ostringstream out;
  unsigned int value = 7;
  out << value;
}
static_assert(sizeof(&ostringstream_unsigned_anchor) > 0, "ostringstream unsigned body anchor");
