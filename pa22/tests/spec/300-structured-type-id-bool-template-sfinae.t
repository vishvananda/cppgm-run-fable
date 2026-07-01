struct input_iterator_tag {};
struct random_access_iterator_tag : input_iterator_tag {};

template<class T>
struct iterator_traits;

template<>
struct iterator_traits<int *> {
  typedef random_access_iterator_tag iterator_category;
};

template<class T>
using iter_category_t = typename iterator_traits<T>::iterator_category;

template<class From, class To>
struct is_convertible {
  static const bool value = false;
};

template<class T>
struct is_convertible<T, input_iterator_tag> {
  static const bool value = true;
};

template<bool B, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

template<class InIter>
using RequireInputIter =
  enable_if_t<is_convertible<iter_category_t<InIter>,
                             input_iterator_tag>::value>;

template<class InIter, class = RequireInputIter<InIter> >
int f(InIter) {
  return 1;
}

int f(int) {
  return 2;
}

int main() {
  int x = 0;
  return f(&x) + f(x) - 3;
}
