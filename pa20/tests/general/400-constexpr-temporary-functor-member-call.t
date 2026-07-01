// VALIDATION: compile-pass
// N3485 focus: 5.19 [expr.const], 7.1.5 [dcl.constexpr]

struct duration
{
  int value;

  constexpr explicit duration(int v) : value(v) {}
  constexpr int count() const { return value; }
};

struct caster
{
  constexpr duration operator()(const duration & input) const
  {
    return duration(input.count());
  }
};

struct wrapper
{
  int value;

  constexpr explicit wrapper(const duration & input)
    : value(caster()(input).count())
  {
  }
};

constexpr int checked_value()
{
  return wrapper(duration(7)).value;
}

static_assert(checked_value() == 7, "temporary functor call result should be constexpr");

int main()
{
  return 0;
}
