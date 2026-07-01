template<bool, class T = void>
struct enable_if {};

template<class T>
struct enable_if<true, T> {
  typedef T type;
};

template<bool B, class T = void>
using enable_if_t = typename enable_if<B, T>::type;

struct Policy {};

template<class T>
struct is_ptr {
  static const bool value = false;
};

template<class T>
struct is_ptr<T*> {
  static const bool value = true;
};

template<class T>
struct iterator_traits;

template<class T>
struct iterator_traits<T*> {
  typedef long difference_type;
};

template<class I>
using iter_diff_t = typename iterator_traits<I>::difference_type;

template<class A, class B>
struct Pair {
  A first;
  B second;
};

template<class P>
struct IterOps {
  template<class Iter>
  using difference_type = iter_diff_t<Iter>;
};

template<class P, class In, class Out, enable_if_t<is_ptr<In>::value, int> = 0>
Pair<In, Out> helper(In first, typename IterOps<P>::template difference_type<In> n, Out result) {
  Pair<In, Out> p = {first, result};
  return p;
}

template<class P, class In, class Out, enable_if_t<!is_ptr<In>::value, int> = 0>
Pair<In, Out> helper(In first, typename IterOps<P>::template difference_type<In> n, Out result) {
  Pair<In, Out> p = {first, result};
  return p;
}

int main() {
  char src_buf[2] = {'a', 'b'};
  char dst_buf[2] = {};
  char* src = src_buf;
  char* dst = dst_buf;
  return helper<Policy>(src, iter_diff_t<char*>(2), dst).second - dst;
}
