// VALIDATION: compile-pass
// N3485 focus: 14.8.2.1 [temp.deduct.call]

template<class T, int N>
int same_extent(T (&)[N], T (&)[N])
{
  return N;
}

int main()
{
  int a[4] = {0, 1, 2, 3};
  int b[4] = {4, 5, 6, 7};
  return same_extent(a, b) == 4 ? 0 : 1;
}
