// VALIDATION: compile-pass
// N3485 focus: 14.5.3 [temp.variadic], 14.8.2 [temp.deduct]

template<class T, class... Args>
int f(T, Args...)
{
  return 9;
}

int main()
{
  return f(1, 2, 3) == 9 ? 0 : 1;
}
