#include <memory>
#include <type_traits>
static_assert(std::is_same<std::allocator_traits<std::allocator<int> >::pointer, int*>::value, "allocator_traits pointer");
namespace {
struct LowIRBlock {};
std::allocator_traits<std::allocator<LowIRBlock> >::pointer p;
}
