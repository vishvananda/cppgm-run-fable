// VALIDATION: compile-pass
// N3485 focus: 8.4.2 [dcl.fct.def.default], 12.1 [class.ctor], 12.4 [class.dtor], 12.8 [class.copy]

struct Empty
{
  Empty();
  ~Empty();
};

Empty::Empty() = default;
Empty::~Empty() = default;

struct Box
{
  int value;

  Box(int v): value(v) {}
  Box(Box&&);
  Box& operator=(Box&&);
  ~Box();
};

Box::Box(Box&&) = default;
Box& Box::operator=(Box&&) = default;
Box::~Box() = default;

int main()
{
  Empty empty;
  Box source(9);
  Box assigned(1);
  assigned = static_cast<Box&&>(source);
  Box moved(static_cast<Box&&>(assigned));
  return moved.value == 9 ? 0 : 1;
}
