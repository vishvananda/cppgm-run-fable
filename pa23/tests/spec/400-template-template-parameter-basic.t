// VALIDATION: compile-pass
// N3485 focus: 14.3.3 [temp.arg.template]

template<typename T>
struct box
{
  static const int value = sizeof(T);
};

template<template<typename> class TT, typename T>
struct apply
{
  static const int value = TT<T>::value;
};

int main()
{
  return apply<box, int>::value == static_cast<int>(sizeof(int)) ? 0 : 1;
}
