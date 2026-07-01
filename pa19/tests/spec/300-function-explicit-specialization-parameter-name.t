// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec], 14.8.1 [temp.arg.explicit]

template<class T>
int value(T primary)
{
  return primary;
}

template<>
int value<int>(int renamed)
{
  return renamed == 7 ? 0 : 1;
}

int main()
{
  return value<int>(7);
}
