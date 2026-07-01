#if __has_include(<__memory/compressed_pair.h>)
#include <__memory/compressed_pair.h>

struct Alloc {};

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 210000
using compressed_pair_anchor = ::std::__compressed_pair<Alloc, int>;
static_assert(sizeof(compressed_pair_anchor) >= sizeof(int), "compressed pair has storage");
#else
using compressed_pair_anchor = ::std::__compressed_pair_padding<Alloc>;
static_assert(sizeof(compressed_pair_anchor) >= 1, "compressed pair padding is complete");
#endif
#elif __has_include(<bits/shared_ptr_base.h>)
#include <bits/shared_ptr_base.h>

struct Alloc {};

using compressed_pair_anchor = ::std::_Sp_ebo_helper<0, Alloc>;
static_assert(sizeof(compressed_pair_anchor) >= 1, "compressed pair EBO helper is complete");
#else
#error missing stdlib compressed-pair helper
#endif
