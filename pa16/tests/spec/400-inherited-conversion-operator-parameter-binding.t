// VALIDATION: compile-pass
// N3485 focus: 12.3.2 [class.conv.fct]

struct ReadonlyProperty;

struct Function
{
  int value;

  Function()
    : value(0)
  {
  }

  Function(const ReadonlyProperty &)
    : value(1)
  {
  }
};

struct PropertyBase
{
  Function value;

  operator const Function &() const
  {
    return value;
  }
};

struct ReadonlyProperty : PropertyBase
{
};

void execute(const Function &, unsigned long)
{
}

int main()
{
  ReadonlyProperty p_mut;
  const ReadonlyProperty & p = p_mut;
  execute(p, 0);
  return 0;
}
