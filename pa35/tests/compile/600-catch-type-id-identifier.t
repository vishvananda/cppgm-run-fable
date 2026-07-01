#include <stdexcept>
#include <type_traits>
static_assert(std::is_base_of<std::exception, std::out_of_range>::value, "out_of_range : exception");
void catch_out_of_range_anchor()
{
  try { throw std::out_of_range("x"); } catch(std::out_of_range) {}
}
static_assert(sizeof(&catch_out_of_range_anchor) > 0, "out_of_range catch body anchor");
