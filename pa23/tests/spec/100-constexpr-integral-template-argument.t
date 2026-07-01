// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype]

constexpr int k = 3;

template<int N>
struct box
{
  static const int value = N;
};

int main()
{
  return box<k + 1>::value == 4 ? 0 : 1;
}
