// VALIDATION: compile-pass
// Assignment should use the exact non-template conversion function first.

template<class T, class A, class B>
struct box {
  int value;
  box() : value(0) {}
  box(int v) : value(v) {}
  box &operator=(box other) { value = other.value; return *this; }
};

template<class T>
struct source {
  typedef box<T, long, char> impl_type;
  int value;
  source(int v) : value(v) {}

  operator impl_type() const { return impl_type(value); }

  template<template<class, class, class> class Seq, class U, class A, class B>
  operator Seq<U, A, B>() const { return Seq<U, A, B>(value + 1); }
};

int main()
{
  box<int, long, char> b;
  b = source<int>(7);
  return b.value == 7 ? 0 : 1;
}
