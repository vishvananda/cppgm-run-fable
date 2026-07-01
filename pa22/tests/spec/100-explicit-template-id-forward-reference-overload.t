// VALIDATION: compile-pass
// N3485 focus: 14.8.1 [temp.arg.explicit], 14.8.2.1 [temp.deduct.call]

template<class T>
struct remove_reference
{
  typedef T type;
};

template<class T>
struct remove_reference<T &>
{
  typedef T type;
};

template<class T>
struct remove_reference<T &&>
{
  typedef T type;
};

template<class T>
struct is_lvalue_reference
{
  static constexpr bool value = false;
};

template<class T>
struct is_lvalue_reference<T &>
{
  static constexpr bool value = true;
};

template<class T>
T && forward(typename remove_reference<T>::type & t)
{
  return static_cast<T &&>(t);
}

template<class T>
T && forward(typename remove_reference<T>::type && t)
{
  static_assert(!is_lvalue_reference<T>::value, "");
  return static_cast<T &&>(t);
}

int g(int & x)
{
  return forward<int &>(x);
}
