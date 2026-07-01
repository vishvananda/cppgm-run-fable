// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.8.2 [temp.deduct]

template<bool B, class T = void>
struct enable_if
{
};

template<class T>
struct enable_if<true, T>
{
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, class U>
struct is_constructible
{
  static const bool value = true;
};

template<class It>
struct iterator_traits
{
  typedef typename It::reference reference;
};

template<class T>
struct iterator_traits<T *>
{
  typedef T & reference;
};

template<class T>
struct box
{
  typedef T value_type;

  template<class It,
           enable_if_t<is_constructible<value_type,
                                        typename iterator_traits<It>::reference>::value,
                       int> = 0>
  int assign(It, It);

  int call(value_type * p)
  {
    return assign(p, p);
  }
};

template<class T>
template<class It,
         enable_if_t<is_constructible<T,
                                      typename iterator_traits<It>::reference>::value,
                     int> >
int box<T>::assign(It, It)
{
  return 0;
}

int main()
{
  int value = 0;
  box<int> b;
  return b.call(&value);
}
