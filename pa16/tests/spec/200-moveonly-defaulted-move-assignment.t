// VALIDATION: compile-pass
// N3485 focus: 8.4.3 [dcl.fct.def.delete], 12.8 [class.copy]

struct MoveOnly
{
  MoveOnly(int value = 0) : value(value) {}
  MoveOnly(const MoveOnly &) = delete;
  MoveOnly(MoveOnly &&) = default;
  MoveOnly & operator=(const MoveOnly &) = delete;
  MoveOnly & operator=(MoveOnly &&) = default;

  int value;
};

int main()
{
  MoveOnly source(9);
  MoveOnly dest;
  dest = static_cast<MoveOnly &&>(source);
  return dest.value == 9 ? 0 : 1;
}
