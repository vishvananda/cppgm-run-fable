// VALIDATION: compile-pass
// N3485 focus: 14.5.3 [temp.variadic], 14.8.1 [temp.arg.explicit]
// An explicit function-template argument produced by pack expansion must keep
// the already-bound type, even when a local typedef has the same name as the
// type's printable spelling.

struct A {};
struct B {};

template<class T>
struct code
{
  static const int value = 9;
};

template<>
struct code<A>
{
  static const int value = 3;
};

template<>
struct code<B>
{
  static const int value = 5;
};

template<class T>
int add_each()
{
  return code<T>::value;
}

template<class... T>
int add_all()
{
  int result = 0;
  typedef int A[sizeof...(T) + 1];
  (void)A{0, (result = result * 10 + add_each<T>())...};
  return result;
}

int main()
{
  return add_all<A, B>() == 35 ? 0 : 1;
}
