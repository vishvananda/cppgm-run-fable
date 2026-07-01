// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], expression SFINAE where the selected
// function template is deleted.

#include "../support.h"

template<typename T>
void deleted_probe(T) = delete;

template<typename T, typename = void>
struct has_deleted_probe
{
  static const bool value = false;
};

template<typename T>
struct has_deleted_probe<T, void_t<decltype(deleted_probe(declval<T>()))> >
{
  static const bool value = true;
};

template<typename T>
auto call_deleted_probe(T value, int) -> decltype(deleted_probe(value), int())
{
  return 1;
}

template<typename T>
int call_deleted_probe(T, long)
{
  return 2;
}

int main()
{
  return !has_deleted_probe<int>::value &&
         call_deleted_probe(1, 0) == 2 ? 0 : 1;
}
