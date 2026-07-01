// VALIDATION: compile-pass
// N3485 focus: 14.8.3 [temp.over], 7.3.4 [namespace.udir]

namespace lib
{
inline namespace v1
{

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct is_int
{
  static const bool value = false;
};

template<>
struct is_int<int>
{
  static const bool value = true;
};

template<class T, enable_if_t<!is_int<T>::value, int> = 0>
int f(T)
{
  return 2;
}

template<class T, enable_if_t<is_int<T>::value, int> = 0>
int f(T)
{
  return 1;
}

}
}

using namespace lib;

int main()
{
  return f(0) - 1;
}
