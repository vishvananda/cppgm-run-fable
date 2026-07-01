template<class A>
struct traits {
  typedef typename A::pointer pointer;
};

template<class C, bool IsConst>
struct Iter;

template<class C>
struct Iter<C, false> {
  typedef typename C::storage_pointer ptr;
  ptr p;
};

template<class T>
struct Alloc {
  typedef T* pointer;
};

template<class T, class A>
struct Vec;

template<class A>
struct Vec<bool, A> {
  typedef traits<A> storage_traits;
  typedef Iter<Vec, false> pointer;
  typedef pointer iterator;
  typedef typename storage_traits::pointer storage_pointer;
  iterator begin_;
  iterator erase(iterator pos);
};

template<class A>
typename Vec<bool, A>::iterator Vec<bool, A>::erase(iterator pos) {
  begin_ = pos;
  return begin_;
}

int main() {
  Vec<bool, Alloc<int> > v;
  (void)v;
  return 0;
}
