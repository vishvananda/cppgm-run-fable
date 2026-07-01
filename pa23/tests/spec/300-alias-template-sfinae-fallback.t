// VALIDATION: compile-pass
// N3485 focus: 14.5.7 [temp.alias], 14.8.2 [temp.deduct], 14.8.3 [temp.over]

#include "../support.h"

template<typename T>
struct has_member_type
{
  template<typename U>
  static char test(typename U::type *);

  template<typename U>
  static long test(...);

  static const bool value = sizeof(test<T>(0)) == sizeof(char);
};

template<bool B>
using enable_member_t = typename enable_if<B, int>::type;

template<typename T>
enable_member_t<has_member_type<T>::value> choose(T)
{
  return 1;
}

int choose(...)
{
  return 2;
}

struct yes_type
{
  typedef int type;
};

struct no_type
{
};

int main()
{
  return choose(yes_type()) == 1 && choose(no_type()) == 2 ? 0 : 1;
}
