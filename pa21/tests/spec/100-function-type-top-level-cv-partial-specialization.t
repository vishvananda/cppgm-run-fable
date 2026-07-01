// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class T>
struct is_const_match
{
  static const int value = 0;
};

template<class T>
struct is_const_match<const T>
{
  static const int value = 1;
};

static_assert(is_const_match<const int>::value == 1, "");
static_assert(is_const_match<const int()>::value == 0, "");

int main()
{
  return is_const_match<const int()>::value;
}
