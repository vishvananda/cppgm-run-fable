// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem]

template<class T>
struct C
{
  template<class I>
  int f(I first, I last);

private:
  template<class I, class S>
  int g(I first, S sentinel, int n);

  template<class I>
  int g(I first, I last, int n);
};

template<class T>
template<class I>
int C<T>::f(I first, I last)
{
  return g(first, last, 1);
}

template<class T>
template<class I, class S>
int C<T>::g(I first, S, int)
{
  return 1;
}

template<class T>
template<class I>
int C<T>::g(I first, I, int)
{
  return *first;
}

int main()
{
  C<int> c;
  int values[1] = {0};
  return c.f(values, values + 1);
}
