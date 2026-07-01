// VALIDATION: compile-pass

template<class T, class A>
struct box
{
  int f();
};

template<class A>
struct box<bool, A>
{
  int f();
};

template<class T, class A>
int box<T, A>::f()
{
  return 1;
}

template<class A>
int box<bool, A>::f()
{
  return 4;
}

int main()
{
  box<bool, int> *bits = 0;
  return bits->f() == 4 ? 0 : 1;
}
