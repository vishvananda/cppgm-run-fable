// VALIDATION: compile-pass
// N3485 focus: 14.7.3 [temp.expl.spec], 14.5.2 [temp.mem]

template<class T>
struct s
{
  int v;
  s();
  s(int x);
};

template<class T>
s<T>::s() : v(1) {}

template<class T>
s<T>::s(int x) : v(x) {}

template<>
struct s<int>
{
  int v;
  s();
  s(int x);
};

s<int>::s() : v(2) {}

s<int>::s(int x) : v(x + 10) {}

int main()
{
  s<int> b;
  s<int> c(5);
  return b.v + c.v - 17;
}
