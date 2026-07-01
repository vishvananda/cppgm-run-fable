// VALIDATION: compile-pass
// N3485 focus: 14.8.3 [temp.over], 13.3 [over.match]

template<typename T>
int choose(T)
{
  return 1;
}

int choose(int)
{
  return 2;
}

int main()
{
  return choose(3) == 2 && choose(3L) == 1 ? 0 : 1;
}
