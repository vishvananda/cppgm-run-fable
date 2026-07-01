// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], function-type non-type parameters.

int target(short, long, double)
{
  return 7;
}

template<int helper(short, long, double)>
struct tester
{
  static const bool value = true;
};

int main()
{
  return tester<target>::value ? 0 : 1;
}
