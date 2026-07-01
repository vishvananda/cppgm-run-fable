// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec]

template<class T>
int value()
{
  return 1;
}

template<>
int value<int>()
{
  return 2;
}

int main()
{
  return value<int>() == 2 ? 0 : 1;
}
