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
  explicit wrap(Iter q) : p(q) {}

  template<class OtherIter>
  wrap(const wrap<OtherIter> & other) : p(other.p) {}
};

template<class Iter>
wrap<Iter> operator+(wrap<Iter> it, int)
{
  return it;
}

template<class T>
struct mini_vec
{
  typedef T * pointer;
  typedef const T * const_pointer;
  typedef wrap<pointer> iterator;
  typedef wrap<const_pointer> const_iterator;

  iterator begin() { return iterator(); }
  int erase(const_iterator) { return 0; }
};

template<class T>
int call_erase(mini_vec<T> & active, int ai)
{
  return active.erase(active.begin() + ai);
}

int main()
{
  struct local
  {
    int x;
  };

  mini_vec<local> active;
  return call_erase(active, 0);
}
