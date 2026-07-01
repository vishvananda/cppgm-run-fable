// VALIDATION: compile-pass
// N3485 focus: 14.8.2.5 [temp.deduct.type], 13.3.3.1 [over.best.ics]

template<class T>
struct iterator_traits
{
};

template<class T>
struct iterator_traits<const T *>
{
  typedef long difference_type;
};

template<class I>
I next(I value, typename iterator_traits<I>::difference_type count)
{
  return value + count;
}

int main()
{
  const char text[4] = {'a', 'b', 'c', 0};
  return *next(text, 2) - 'c';
}
