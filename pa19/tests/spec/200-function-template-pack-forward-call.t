// VALIDATION: compile-pass
// N3485 focus: 14.5.3 [temp.variadic], 14.8.2 [temp.deduct]

int h(int x, char y)
{
  return x + y;
}

template<class... Args>
int g(Args... args)
{
  return h(args...);
}

int main()
{
  return g(1, 'a') == 98 ? 0 : 1;
}
