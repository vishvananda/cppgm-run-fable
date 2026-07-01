template<class T, class L>
struct components {
  static const int value = 0;
};

template<class T, class L>
struct components<T *, L> {
  static const int value = 1;
};

template<class R, class A0, class L>
struct components<R (*)(A0), L> {
  static const int value = 2;
};

template<class R, class A0, class L>
struct components<R (*)(A0...), L> {
  static const int value = 3;
};

static_assert(components<void (*)(int), void>::value == 2, "");

int main()
{
  return components<void (*)(int), void>::value != 2;
}
