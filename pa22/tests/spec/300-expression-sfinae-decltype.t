// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct]

#include "../support.h"

template<typename T>
auto test_run(T t) -> decltype(t.run(), int())
{
  return 1;
}

int test_run(...)
{
  return 2;
}

struct good
{
  int run()
  {
    return 0;
  }
};

struct bad
{
};

int main()
{
  return test_run(good()) == 1 && test_run(bad()) == 2 ? 0 : 1;
}
