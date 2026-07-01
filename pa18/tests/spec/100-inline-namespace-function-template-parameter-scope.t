// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.8 [temp.fct.spec], 7.3.1 [namespace.def]

namespace s {
inline namespace i {
namespace c {
template<class R, class P>
struct duration {
  duration() {}
  duration(int) {}
};

struct nano {};
struct milli {};
typedef duration<long long, nano> nanoseconds;
typedef duration<long long, milli> milliseconds;
}
}
}

namespace s {
inline namespace i {
namespace c {
template<class R1, class P1, class R2, class P2>
bool less(const duration<R1, P1> &, const duration<R2, P2> &)
{
  return true;
}

template<class R1, class P1, class R2, class P2>
bool greater(const duration<R1, P1> & a, const duration<R2, P2> & b)
{
  return less(b, a);
}
}
}
}

bool f(s::c::nanoseconds elapsed)
{
  return s::c::greater(elapsed, s::c::milliseconds(128));
}

int main()
{
  return f(s::c::nanoseconds()) ? 0 : 1;
}
