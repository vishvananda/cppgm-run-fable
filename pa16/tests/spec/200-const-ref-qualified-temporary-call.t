// VALIDATION: compile-pass
// N3485 focus: 9.3.1 [class.mfct.non-static]

struct Fun
{
  int operator()(int x) const &
  {
    return x + 1;
  }
};

int main()
{
  return Fun()(4) == 5 ? 0 : 1;
}
