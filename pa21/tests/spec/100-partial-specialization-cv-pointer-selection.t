// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<typename T>
struct pick
{
  static const int value = 0;
};

template<typename T>
struct pick<T *>
{
  static const int value = 1;
};

template<typename T>
struct pick<const T *>
{
  static const int value = 2;
};

int main()
{
  return pick<int *>::value == 1 && pick<const int *>::value == 2 ? 0 : 1;
}
