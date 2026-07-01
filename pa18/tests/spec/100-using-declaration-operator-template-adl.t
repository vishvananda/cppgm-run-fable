// VALIDATION: compile-pass
// N3485 focus: 3.4.2 [basic.lookup.argdep], 7.3.3 [namespace.udecl]

namespace lib
{
struct seq {};

namespace operators
{
template<class L, class R>
bool operator==(const L&, const R&)
{
  return true;
}
}

using operators::operator==;
}

int main()
{
  lib::seq a;
  lib::seq b;
  return a == b ? 0 : 1;
}
