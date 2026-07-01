// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type]
// Deduction against a class template specialization may match a prefix of the
// template arguments while remaining arguments come from defaults, including
// through an overloaded compound-assignment operator template.

template<class T>
struct less
{
};

template<class T>
struct vec
{
  typedef T value_type;
};

template<class T, class Container = vec<T>, class Compare = less<typename Container::value_type> >
struct queue
{
};

template<class T, class Container, class U>
int operator+=(queue<T, Container> &, U)
{
  return 7;
}

int main()
{
  queue<int> q;
  return (q += 1) == 7 ? 0 : 1;
}
