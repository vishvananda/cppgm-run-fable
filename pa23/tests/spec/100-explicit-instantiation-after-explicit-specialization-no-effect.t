// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit], 14.7.3 [temp.expl.spec]

template<typename T>
int value(T)
{
  return 1;
}

template<>
int value<int>(int)
{
  return 7;
}

template int value<int>(int);

int main()
{
  return value(0) == 7 ? 0 : 1;
}
