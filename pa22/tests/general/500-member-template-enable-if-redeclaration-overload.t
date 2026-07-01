// VALIDATION: compile-pass
// Body-less member-template declarations with identical function parameters but
// different defaulted non-type enable-if parameters are distinct templates.

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T>
struct is_ptr {
  static const bool value = false;
};

template<class T>
struct is_ptr<T *> {
  static const bool value = true;
};

template<class T>
struct box {
  template<class U, enable_if_t<!is_ptr<U>::value, int> = 0>
  int pick(U, U);

  template<class U, enable_if_t<is_ptr<U>::value, int> = 0>
  int pick(U, U);

  int run(T first, T last)
  {
    return pick(first, last);
  }
};

template<class T>
template<class U, enable_if_t<!is_ptr<U>::value, int>>
int box<T>::pick(U, U)
{
  return 1;
}

template<class T>
template<class U, enable_if_t<is_ptr<U>::value, int>>
int box<T>::pick(U, U)
{
  return 2;
}

int main()
{
  int value = 0;
  box<int *> b;
  return b.run(&value, &value) == 2 ? 0 : 1;
}
