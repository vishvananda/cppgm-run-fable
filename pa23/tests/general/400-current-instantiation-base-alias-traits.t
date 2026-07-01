// PA23 integration: an alias-template base whose target is a class template
// must materialize the selected class specialization after dependent trait
// members are resolved for the current instantiation.
template<class A, class B> struct same {
  static const bool value = false;
};

template<class A> struct same<A, A> {
  static const bool value = true;
};

struct random_access_iterator_tag {};

template<class Category, class T, class Distance, class Pointer, class Reference>
struct iterator {
  typedef Category iterator_category;
  typedef T value_type;
  typedef Distance difference_type;
  typedef Pointer pointer;
  typedef Reference reference;
};

template<class Iter> struct iterator_traits;

template<class T>
struct raw_iter : iterator<random_access_iterator_tag, T, long, T *, T &> {};

template<class T>
struct iterator_traits<raw_iter<T> > {
  typedef typename raw_iter<T>::iterator_category iterator_category;
  typedef typename raw_iter<T>::value_type value_type;
  typedef typename raw_iter<T>::difference_type difference_type;
  typedef typename raw_iter<T>::pointer pointer;
  typedef typename raw_iter<T>::reference reference;
};

template<class Derived, class Category, class T, class Distance, class Pointer, class Reference>
using iterator_base = iterator<Category, T, Distance, Pointer, Reference>;

template<class Iter>
struct reverse_iterator
  : iterator_base<reverse_iterator<Iter>,
                  typename iterator_traits<Iter>::iterator_category,
                  typename iterator_traits<Iter>::value_type,
                  typename iterator_traits<Iter>::difference_type,
                  typename iterator_traits<Iter>::pointer,
                  typename iterator_traits<Iter>::reference> {
  typedef typename reverse_iterator::iterator_category category;
};

int main() {
  return same<reverse_iterator<raw_iter<int> >::category,
              random_access_iterator_tag>::value ? 0 : 1;
}
