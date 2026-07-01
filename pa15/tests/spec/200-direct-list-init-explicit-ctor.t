// VALIDATION: compile-pass
// N3485 focus: 8.5.4 [dcl.init.list]

struct S
{
  explicit S(int value) : value(value) {}
  int value;
};

int main()
{
  S s{4};
  return s.value == 4 ? 0 : 1;
}
