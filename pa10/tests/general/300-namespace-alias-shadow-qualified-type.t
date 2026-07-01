namespace target {
typedef int duration;
}

namespace alias = target;

void f()
{
  int alias;
  alias::duration d;
}
