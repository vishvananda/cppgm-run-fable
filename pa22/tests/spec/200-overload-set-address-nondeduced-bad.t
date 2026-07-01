// VALIDATION: compile-fail
// N3485 focus: 14.8.2.2 [temp.deduct.funcaddr]

int choose(int)
{
  return 1;
}

double choose(double)
{
  return 2.0;
}

template<typename R, typename A>
R use(R (*fn)(A))
{
  return fn(A());
}

int main()
{
  return use(choose);
}
