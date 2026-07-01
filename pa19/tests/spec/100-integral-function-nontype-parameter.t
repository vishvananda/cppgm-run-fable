// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.2 [temp.arg.nontype]

template<int N>
int f()
{
  return N;
}

int main()
{
  return f<3>() == 3 ? 0 : 1;
}
