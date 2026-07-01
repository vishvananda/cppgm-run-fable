template<bool B, class T>
struct enable_if {
  typedef T type;
};

template<class T>
struct accepts {
  static const bool value = true;
};

template<bool B, class T>
using enable_if_t = typename enable_if<B, T>::type;

template<class T, class Alloc>
struct shell {};

template<class Alloc>
struct shell<bool, Alloc> {
  using allocator_type = Alloc;

  template<class It, enable_if_t<accepts<It>::value, int> = 0>
  shell(It first, It last, const shell<bool, Alloc>::allocator_type& alloc);
};

template<class Alloc>
template<class It, enable_if_t<accepts<It>::value, int> >
shell<bool, Alloc>::shell(It first, It last, const allocator_type& alloc) {
  (void)first;
  (void)last;
  (void)alloc;
}

int main() {
  int alloc = 0;
  shell<bool, int> value(1, 2, alloc);
  return 0;
}
