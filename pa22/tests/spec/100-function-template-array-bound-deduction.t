// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct]

template<class T, int N>
int array_size(T (&)[N])
{
  return N;
}

int main()
{
  int a[3] = {0, 1, 2};
  return array_size(a) == 3 ? 0 : 1;
}
