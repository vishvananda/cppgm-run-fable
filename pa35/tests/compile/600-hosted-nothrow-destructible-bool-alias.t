#include <type_traits>
#include <utility>

struct Entry {};

typedef std::is_nothrow_destructible<std::pair<Entry*, unsigned long> > trait;

static_assert(sizeof(trait) > 0, "nothrow destructible trait is complete");
