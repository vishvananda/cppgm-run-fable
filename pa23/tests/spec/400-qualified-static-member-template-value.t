// VALIDATION: compile-pass
// N3485 focus: 14.6.2.1 [temp.dep.type], 14.3.2 [temp.arg.nontype]

template<class A, class B>
struct is_same
{
  static constexpr bool value = false;
};

template<class A>
struct is_same<A, A>
{
  static constexpr bool value = true;
};

template<class T, class U>
int f()
{
  return is_same<T, U>::value ? 1 : 0;
}

int main()
{
  return f<int, int>() == 1 ? 0 : 1;
}
