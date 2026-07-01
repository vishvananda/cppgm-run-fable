// VALIDATION: compile-pass

struct tracked
{
  int value;
};

struct wrapper
{
  int value;

  wrapper(const tracked& p) : value(p.value) {}
};

int read_ptr(const wrapper *p)
{
  return p->value;
}

int main()
{
  tracked p;
  p.value = 9;
  return read_ptr(&static_cast<const wrapper&>(p)) - 9;
}
