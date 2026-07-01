template<bool B>
struct enable_if
{
};

template<>
struct enable_if<true>
{
  typedef void type;
};

template<class T>
struct is_custom
{
  static const bool value = false;
};

template<class A>
struct base
{
  static int f(void *)
  {
    return 1;
  }

  template<class T>
  static int f(T *)
  {
    return 2;
  }
};

template<class A>
struct derived : base<A>
{
  using base<A>::f;

  template<class T>
  static typename enable_if<is_custom<T>::value>::type f(T)
  {
  }
};

int main()
{
  int value = 0;
  return derived<int>::f(&value);
}
