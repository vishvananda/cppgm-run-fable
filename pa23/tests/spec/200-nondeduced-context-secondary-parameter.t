// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type]

template<class T>
struct wrap
{
  typedef T type;
};

template<class T>
int choose(typename wrap<T>::type, T)
{
  return 1;
}

int main()
{
  return choose(1, 2) == 1 ? 0 : 1;
}
