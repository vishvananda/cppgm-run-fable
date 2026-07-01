// VALIDATION: compile-pass
// N3485 focus: 10.3 [class.virtual]

struct Base
{
  virtual int f() noexcept
  {
    return 1;
  }

  virtual ~Base() noexcept {}
};

struct Derived : Base
{
  int f() noexcept override
  {
    return Base::f();
  }
};

int main()
{
  Derived d;
  return d.f() == 1 ? 0 : 1;
}
