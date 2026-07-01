// VALIDATION: compile-pass
// N3485 focus: 14.8.2.2 [temp.deduct.funcaddr]

int square(int x)
{
  return x * x;
}

double square(double x)
{
  return x * x;
}

template<typename T>
T apply(T (*fn)(T), T value)
{
  return fn(value);
}

int main()
{
  return apply(square, 4) == 16 ? 0 : 1;
}
