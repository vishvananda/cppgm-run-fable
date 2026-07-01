// N3485 focus: 7.3.2 [namespace.alias], 6 [stmt.stmt], 5.2.2 [expr.call]
// VALIDATION: compile-pass

namespace outer {
namespace inner {

int value(int x)
{
  return x + 1;
}

}
}

int main()
{
  namespace local = outer::inner;
  return local::value(2) == 3 ? 0 : 1;
}
