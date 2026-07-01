// VALIDATION: compile-pass
// N3485 focus: 14.6.3 [temp.nondep]

int target(int)
{
  return 1;
}

template<typename T>
int call_target()
{
  return target(0);
}

double target(double);

int main()
{
  return call_target<int>() == 1 ? 0 : 1;
}
