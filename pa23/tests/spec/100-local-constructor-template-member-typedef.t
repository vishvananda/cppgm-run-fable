// VALIDATION: compile-pass
// N3485 focus: 14.5.2 [temp.mem], 14.6.1 [temp.local], 14.8 [temp.fct.spec]

template<class T>
struct iterator_traits;

template<class T>
struct iterator_traits<T *>
{
  typedef T & reference;
};

template<class Iter>
struct wrap
{
  typedef typename iterator_traits<Iter>::reference reference;
  Iter p;

  wrap() : p(0) {}

  template<class OtherIter>
  wrap(const wrap<OtherIter> & other) : p(other.p) {}
};

template<class T>
struct mini_vec
{
  typedef wrap<T *> iterator;
  typedef wrap<const T *> const_iterator;
};

int main()
{
  struct local
  {
    int x;
  };

  mini_vec<local>::iterator it;
  mini_vec<local>::const_iterator cit(it);
  (void)cit;
  return 0;
}
