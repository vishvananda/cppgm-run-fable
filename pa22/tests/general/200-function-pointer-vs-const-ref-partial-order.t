// VALIDATION: compile-pass
// N3485 focus: 14.8.2.4 [temp.deduct.partial], 13.3.3 [over.match.best]

template<class T>
int pick(const T &)
{
  return 1;
}

template<class T>
int pick(T *)
{
  return 2;
}

int f(unsigned long)
{
  return 0;
}

int main()
{
  return pick(&f) - 2;
}
