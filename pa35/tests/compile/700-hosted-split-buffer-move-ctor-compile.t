#include <deque>
#include <type_traits>
static_assert(std::is_same<std::deque<int>::value_type, int>::value, "<deque> value_type");
struct CalcToken {};
void split_buffer_move_anchor()
{
#ifdef _LIBCPP_VERSION
#if _LIBCPP_VERSION >= 220000
  typedef std::__1::__split_buffer<CalcToken *, std::__1::allocator<CalcToken *>, std::__1::__split_buffer_pointer_layout> Buffer;
#else
  typedef std::__1::__split_buffer<CalcToken *, std::__1::allocator<CalcToken *> > Buffer;
#endif
  Buffer from;
  Buffer to(std::move(from));
#endif
}
static_assert(sizeof(&split_buffer_move_anchor) > 0, "split_buffer move body anchor");
