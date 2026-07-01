// VALIDATION: compile-pass
// N3485 focus: 14.8.3 [temp.over]

template<class T>
int f(T)
{
  return 1;
}

template<class T>
int f(T *)
{
  return 2;
}

template<class T>
int f(const T *)
{
  return 3;
}

int main()
{
  const int * p = 0;
  return f(p) == 3 ? 0 : 1;
}
