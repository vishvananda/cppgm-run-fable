struct Base
{
  virtual ~Base() noexcept {}
  virtual void destroy() noexcept {}
};

struct Derived : Base
{
  ~Derived() noexcept override {}

  void destroy() noexcept override
  {
    this->~Derived();
  }
};

int main()
{
  Derived d;
  d.destroy();
  return 0;
}
