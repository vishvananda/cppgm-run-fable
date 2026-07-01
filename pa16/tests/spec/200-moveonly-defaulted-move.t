// VALIDATION: compile-pass
// N3485 focus: 8.4.3 [dcl.fct.def.delete], 12.8 [class.copy]

struct MoveOnly
{
  MoveOnly() : value(5) {}
  MoveOnly(const MoveOnly &) = delete;
  MoveOnly(MoveOnly &&) = default;

  int value;
};

int main()
{
  MoveOnly source;
  MoveOnly dest(static_cast<MoveOnly &&>(source));
  return dest.value == 5 ? 0 : 1;
}
