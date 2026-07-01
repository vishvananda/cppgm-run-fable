template<class T>
struct decay {
  typedef T type;
};

template<class T>
using decay_t = typename decay<T>::type;

template<class T, class U = decay_t<T> >
struct dependent_default {
  typedef U type;
};

template<class T, class U = decay_t<T> >
using dependent_default_t = typename dependent_default<T, U>::type;

template<class T>
struct outer {
  typedef dependent_default_t<T> stored;
};

template<class T>
typename outer<T>::stored make_value(T value) {
  return value;
}

int main() {
  return make_value(3) == 3 ? 0 : 1;
}
