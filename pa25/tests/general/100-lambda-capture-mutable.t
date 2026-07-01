// VALIDATION: compile-pass
// N3485 focus: 5.1.2 [expr.prim.lambda]

int main()
{
  int x = 2;
  auto f = [x]() mutable
  {
    x += 3;
    return x;
  };

  return f() == 5 ? 0 : 1;
}
