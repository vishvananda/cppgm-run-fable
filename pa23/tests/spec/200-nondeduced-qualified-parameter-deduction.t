// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type]

template<class T>
struct a
{
  typedef int b;
};

template<class T>
int f(typename a<T>::b, a<T>)
{
  return 0;
}

int main()
{
  a<int> x;
  return f(0, x);
}
