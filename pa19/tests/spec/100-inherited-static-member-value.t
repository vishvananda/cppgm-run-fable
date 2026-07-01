// VALIDATION: compile-pass
// N3485 focus: 14.3.2 [temp.arg.nontype]

template<class T, T v>
struct integral_constant
{
  static const T value = v;
};

template<class T>
struct is_const : integral_constant<bool, false>
{
};

int main()
{
  return is_const<char>::value ? 1 : 0;
}
