// VALIDATION: compile-pass
// N3485 focus: 14.5.5 [temp.class.spec]

template<class C>
struct range_const_iterator
{
  static const int selected = 0;
};

template<class T, unsigned long N>
struct range_const_iterator<T[N]>
{
  typedef const T *type;
  static const int selected = 1;
};

int main()
{
  return range_const_iterator<const int[5]>::selected == 1 ? 0 : 1;
}
