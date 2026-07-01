// VALIDATION: compile-pass
// N3485 focus: 12.8 [class.copy]

struct Inner
{
  Inner() noexcept : value(7) {}
  ~Inner() noexcept {}

  Inner(const Inner &) = delete;
  Inner & operator=(const Inner &) = delete;

  Inner(Inner && other) noexcept : value(other.value)
  {
    other.value = -1;
  }

  Inner & operator=(Inner && other) noexcept
  {
    if(this != &other) {
      value = other.value;
      other.value = -1;
    }
    return *this;
  }

  int value;
};

struct Outer
{
  Inner inner;
};

int main()
{
  Outer source;
  Outer moved(static_cast<Outer &&>(source));
  return moved.inner.value == 7 ? 0 : 1;
}
