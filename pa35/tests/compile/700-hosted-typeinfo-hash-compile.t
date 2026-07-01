#include <typeinfo>
#include <cstddef>
#include <type_traits>
static_assert(std::is_same<decltype(typeid(int).hash_code()), std::size_t>::value,
              "std::type_info::hash_code() -> std::size_t");
bool typeinfo_hash_anchor()
{
  const std::type_info & int_type = typeid(int);
  return !(int_type == typeid(double)) &&
         int_type == typeid(int) &&
         int_type.hash_code() == typeid(int).hash_code();
}
static_assert(sizeof(&typeinfo_hash_anchor) > 0, "type_info hash body anchor");
