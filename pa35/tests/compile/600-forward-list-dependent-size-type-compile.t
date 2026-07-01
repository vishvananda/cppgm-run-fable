#include <forward_list>
#include <memory>
#include <type_traits>

using list_type = std::forward_list<int>;

static_assert(std::is_same<list_type::value_type, int>::value,
              "forward_list value_type");
static_assert(
    std::is_same<
        list_type::size_type,
        std::allocator_traits<list_type::allocator_type>::size_type>::value,
    "forward_list size_type comes from allocator_traits");
