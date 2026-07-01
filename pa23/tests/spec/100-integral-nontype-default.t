// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.2 [temp.arg.nontype]

template<int N = 0>
struct box
{
  static const int value = N;
};

int main()
{
  return box<>::value;
}
