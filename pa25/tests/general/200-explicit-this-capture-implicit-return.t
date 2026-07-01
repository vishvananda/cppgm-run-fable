// VALIDATION: compile-pass
// N3485 focus: 5.1.2 [expr.prim.lambda]

struct Box
{
  Box(int value) : value(value) {}

  int run()
  {
    int (*unused)() = 0;
    (void)unused;
    return [this]() { return value; }();
  }

  int value;
};

int main()
{
  Box box(7);
  return box.run() == 7 ? 0 : 1;
}
