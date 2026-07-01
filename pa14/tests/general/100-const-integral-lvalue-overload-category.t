int select(int const &)
{
  return 0;
}

int select(int const &&)
{
  return 1;
}

int const *addr(int const &value)
{
  return &value;
}

void addr(int const &&) = delete;

int main()
{
  int const value = 0;
  return select(value) == 0 && addr(value) == &value ? 0 : 1;
}
// VALIDATION: compile-pass
