// VALIDATION: compile-pass
// Defaulted non-type parameters whose type depends on enable_if should be
// substituted after deduction so only the viable overload remains.

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<class T>
struct has_value
{
  static const bool value = false;
};

struct selected
{
};

template<>
struct has_value<selected>
{
  static const bool value = true;
};

template<class T, typename enable_if<has_value<T>::value, int>::type = 0>
int choose(T)
{
  return 7;
}

template<class T, typename enable_if<!has_value<T>::value, int>::type = 0>
int choose(T)
{
  return 11;
}

int main()
{
  selected value;
  return choose(value) == 7 ? 0 : 1;
}
