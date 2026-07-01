// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.2 [temp.arg.nontype]

template<class T, T v>
struct integral_constant
{
  static const T value = v;
};

int main()
{
  return integral_constant<int, 7>::value == 7 ? 0 : 1;
}
