// VALIDATION: compile-pass
// N3485 focus: 14.5.3 [temp.variadic], 14.8.2 [temp.deduct]

template<class... Args>
int f(Args...)
{
  return 7;
}

int main()
{
  return f(1, 2, 3) == 7 ? 0 : 1;
}
