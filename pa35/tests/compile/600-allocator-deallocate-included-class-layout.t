#include <memory>
#include <type_traits>
#include "600-allocator-deallocate-included-class-layout.h"
static_assert(alignof(included_layout_type) > 0, "included type has alignment");
static_assert(std::is_same<std::allocator<included_layout_type>::value_type, included_layout_type>::value, "allocator value_type");
void use_allocator(included_layout_type * ptr, unsigned long n)
{ std::allocator<included_layout_type> alloc; alloc.deallocate(ptr, n); }
static_assert(sizeof(&use_allocator) > 0, "allocator deallocate body anchor");
