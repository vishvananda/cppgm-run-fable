// VALIDATION: compile-pass
// N3485 focus: 14.6.2 [temp.dep], 7.3.3 [namespace.udecl]

template<typename T>
struct Base
{
  int f(int)
  {
    return 1;
  }
};

template<typename T>
struct Derived : Base<T>
{
  using Base<T>::f;

  int f(double)
  {
    return 2;
  }
};

int main()
{
  Derived<int> d;
  return d.f(1) == 1 && d.f(1.5) == 2 ? 0 : 1;
}
