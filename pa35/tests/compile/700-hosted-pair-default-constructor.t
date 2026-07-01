#include <utility>

using hosted_pair = std::pair<unsigned long, unsigned long>;
using default_constructed_pair = decltype(hosted_pair());

static_assert(__is_same(default_constructed_pair, hosted_pair), "pair default construction is unambiguous");
