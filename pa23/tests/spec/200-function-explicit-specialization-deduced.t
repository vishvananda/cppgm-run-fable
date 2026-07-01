// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec], 14.8.1 [temp.arg.explicit]

template<class T>
int f(T)
{
  return 1;
}

template<>
int f<int>(int)
{
  return 2;
}

int main()
{
  return f(0) == 2 ? 0 : 1;
}
