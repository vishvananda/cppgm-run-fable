// VALIDATION: compile-pass
// N3485 focus: 14.7.2 [temp.explicit]

namespace ns
{
  template<class T, class Alloc = void>
  struct box
  {
    static int value();
  };

  template<class T, class Alloc>
  int box<T, Alloc>::value()
  {
    return 17;
  }
}

struct empty {};

template class ::ns::box<empty>;

int main()
{
  return ::ns::box<empty>::value() == 17 ? 0 : 1;
}
