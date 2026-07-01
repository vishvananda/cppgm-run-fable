// VALIDATION: compile-pass
// N3485 focus: 8.4.3 [dcl.fct.def.delete], 14.8.2 [temp.deduct],
// and expression SFINAE for deleted function templates named as call callees.

#include "../support.h"

struct object
{
};

struct property
{
};

template<typename T, typename P>
void deleted_query(T, P) = delete;

template<typename T, typename P, typename = void>
struct has_deleted_query
{
  static const bool value = false;
};

template<typename T, typename P>
struct has_deleted_query<T, P,
                         void_t<decltype(deleted_query(declval<T>(),
                                                       declval<P>()))> >
{
  static const bool value = true;
};

template<typename T, typename P>
auto call_deleted_query(T value, P prop, int)
    -> decltype(deleted_query(value, prop), int())
{
  return 1;
}

template<typename T, typename P>
int call_deleted_query(T, P, long)
{
  return 2;
}

int main()
{
  return !has_deleted_query<object, property>::value &&
         call_deleted_query(object(), property(), 0) == 2 ? 0 : 1;
}
