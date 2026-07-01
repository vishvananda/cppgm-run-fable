// VALIDATION: compile-pass
// Boost.Asio reduction: a hidden friend function template declared inside a
// class template can use the current specialization name in an enable_if
// return type.

template <bool B, class T>
struct enable_if
{
};

template <class T>
struct enable_if<true, T>
{
  typedef T type;
};

template <class A, class B>
struct is_same
{
  static const bool value = false;
};

template <class A>
struct is_same<A, A>
{
  static const bool value = true;
};

template <class T>
struct any_executor
{
  template <class AnyExecutor>
  friend typename enable_if<is_same<AnyExecutor, any_executor>::value, bool>::type
  operator==(const AnyExecutor&, int)
  {
    return true;
  }
};

int main()
{
  any_executor<int> ex;
  return ex == 0 ? 0 : 1;
}
