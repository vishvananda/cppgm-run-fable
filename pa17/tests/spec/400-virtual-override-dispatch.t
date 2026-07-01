// VALIDATION: compile-pass
// N3485 focus: 10.3 [class.virtual]

struct Base
{
  virtual int f() const noexcept
  {
    return 1;
  }

  virtual ~Base() noexcept {}
};

struct Derived : Base
{
  int f() const noexcept override
  {
    return 2;
  }
};

int main()
{
  Derived d;
  Base & b = d;
  return b.f() == 2 ? 0 : 1;
}
