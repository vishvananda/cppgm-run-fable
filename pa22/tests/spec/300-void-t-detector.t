// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct]

#include "../support.h"

template<typename T, typename = void>
struct has_iterator_category
{
  static const bool value = false;
};

template<typename T>
struct has_iterator_category<T, void_t<typename T::iterator_category> >
{
  static const bool value = true;
};

struct iter_like
{
  typedef int iterator_category;
};

struct plain
{
};

int main()
{
  return has_iterator_category<iter_like>::value &&
                 !has_iterator_category<plain>::value
             ? 0
             : 1;
}
