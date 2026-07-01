// VALIDATION: compile-pass
// N3485 focus: 14.8.2 [temp.deduct], 14.8.3 [temp.over]

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

template<class T, unsigned N, enable_if_t<is_int<T>::value, int> = 0>
int guarded_extent(T (&)[N], T (&)[N])
{
  return N;
}

int main()
{
  int a[5] = {0, 1, 2, 3, 4};
  int b[5] = {5, 6, 7, 8, 9};
  return guarded_extent(a, b) - 5;
}
