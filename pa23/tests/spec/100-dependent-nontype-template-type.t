// VALIDATION: compile-pass
// N3485 focus: 14.1 [temp.param], 14.3.2 [temp.arg.nontype], 14.6.2.1 [temp.dep.type]

template<class Cp>
struct holder
{
  typedef int storage_type;
};

template<class Cp, bool IsConst, typename Cp::storage_type = 0>
struct bit_iterator
{
  static int value() { return IsConst ? 1 : 0; }
};

int main()
{
  return bit_iterator<holder<void>, true>::value() == 1 ? 0 : 1;
}
