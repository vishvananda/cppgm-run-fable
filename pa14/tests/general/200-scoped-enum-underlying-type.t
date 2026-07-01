// VALIDATION: compile-pass
// N3485 focus: 7.2 [dcl.enum]

enum class Flag : unsigned short
{
  A = 1,
  B = 2
};

int main()
{
  Flag f = Flag::B;
  return static_cast<unsigned short>(f) == 2 ? 0 : 1;
}
