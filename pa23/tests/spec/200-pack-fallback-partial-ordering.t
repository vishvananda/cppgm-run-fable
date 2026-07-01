// VALIDATION: compile-pass
// N3485 focus: 14.8.3 [temp.over]

template<typename T>
int pick(T, T)
{
  return 1;
}

template<typename T, typename... Rest>
int pick(T, Rest...)
{
  return 2;
}

int main()
{
  return pick(1, 1) == 1 && pick(1, 2, 3) == 2 ? 0 : 1;
}
