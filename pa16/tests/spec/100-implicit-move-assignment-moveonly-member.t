// VALIDATION: compile-pass
// N3485 focus: 12.8 [class.copy]

struct MoveOnly
{
  MoveOnly(int value = 0) : value(value) {}
  MoveOnly(const MoveOnly &) = delete;
  MoveOnly(MoveOnly && other) : value(other.value)
  {
    other.value = 0;
  }

  MoveOnly & operator=(const MoveOnly &) = delete;
  MoveOnly & operator=(MoveOnly && other)
  {
    if(this != &other) {
      value = other.value;
      other.value = 0;
    }
    return *this;
  }

  int value;
};

struct Outer
{
  MoveOnly inner;
};

Outer make()
{
  Outer out;
  out.inner.value = 7;
  return out;
}

int main()
{
  Outer target;
  target = make();
  return target.inner.value == 7 ? 0 : 1;
}
