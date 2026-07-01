// VALIDATION: compile-pass
// N3485 focus: default template-arguments across redeclarations

template<typename T = int>
struct box;

template<typename T>
struct box
{
  static const int value = 1;
};

int main()
{
  box<> b;
  (void)b;
  return box<>::value == 1 ? 0 : 1;
}
